// related headers

// c sys headers
#include <cstdio>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <climits>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// 3rd party headers
#include <uwebsockets/App.h>
#include <cassandra.h>
#include <librdkafka/rdkafkacpp.h>

// project headers
#include "CassandraDeleters.hh"
#include "proto/signaling.pb.h"


namespace
{

// ============================================================
// Per-connection state. uWebSockets allocates one of these
// inside each WS handle via the template parameter on `ws<>()`.
// Access via `ws->getUserData()`.
// ============================================================

struct Session
{

  std::string user_id { };
  std::string session_id { };
  bool authenticated { false };

};

// uWS template params: <SSL, isServer, UserData>
using WebSocket = uWS::WebSocket<false, true, Session>;


// ============================================================
// Process-wide state. One gateway instance owns one of these.
// ============================================================

struct Gateway
{

  // Non-owning: the App lives on the stack in main(); we just need a
  // handle so the Kafka consumer thread can call app->publish().
  uWS::App* app { nullptr };

  // Scylla. Declaration order matters for destruction: members are
  // destroyed in reverse, so prepareds release before the session and
  // the session releases before the cluster (which is what the
  // DataStax driver requires).
  OpenSocialNet::Signaling::CassClusterPtr cass_cluster { };
  OpenSocialNet::Signaling::CassSessionPtr cass_session { };
  OpenSocialNet::Signaling::CassUuidGenPtr uuid_gen { };
  OpenSocialNet::Signaling::CassPreparedPtr prep_insert_message { };
  OpenSocialNet::Signaling::CassPreparedPtr prep_fetch_history { };
  OpenSocialNet::Signaling::CassPreparedPtr prep_user_channels { };

  // Kafka
  std::unique_ptr<RdKafka::Producer> producer { };
  std::string topic_name { "chat_events" };

  // Lifecycle
  std::atomic<bool> running { true };

};

Gateway gateway { };


// ============================================================
// Generic helpers
// ============================================================

void send_envelope(WebSocket* ws, const signaling::Envelope& envelope)
{

  std::string buf { };
  envelope.SerializeToString(&buf);
  ws->send(buf, uWS::OpCode::BINARY);

}

void send_error(WebSocket* ws, uint32_t code, std::string_view msg)
{

  signaling::Envelope envelope { };
  // env.mutable_error() returns a non-owning ptr -- the Envelope owns it.
  auto* e = envelope.mutable_error();
  e->set_code(code);
  e->set_message(std::string(msg));
  send_envelope(ws, envelope);

}

std::string make_session_id()
{

  // 16 hex chars from a thread-local PRNG. Real impl: UUIDv4 from CSPRNG.
  thread_local std::mt19937_64 rng { std::random_device { }() };
  char buf[17] { };
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(rng()));
  return std::string(buf, 16);

}


// ============================================================
// Scylla
// ============================================================

[[noreturn]] void scylla_die(const char* what, CassFuture* f)
{

  // msg points into the future's internal buffer -- borrowed view, so a
  // string_view is the right wrapper (not unique_ptr).
  const char* msg { nullptr };
  size_t msg_len { 0 };
  cass_future_error_message(f, &msg, &msg_len);
  std::cerr << what << " failed: " << std::string_view { msg, msg_len } << '\n';
  std::exit(1);

}

void scylla_init()
{

  gateway.cass_cluster.reset(cass_cluster_new());
  cass_cluster_set_contact_points(gateway.cass_cluster.get(), "127.0.0.1");

  gateway.cass_session.reset(cass_session_new());
  gateway.uuid_gen.reset(cass_uuid_gen_new());

  OpenSocialNet::Signaling::CassFuturePtr fut { cass_session_connect_keyspace(gateway.cass_session.get(), gateway.cass_cluster.get(), "opensocialnet") };
  if (cass_future_error_code(fut.get()) != CASS_OK) scylla_die("scylla connect", fut.get());

  // Prepared statements: parsed once on Scylla, cached, reused per call.
  // Server keeps a md5 -> AST map; we just send the hash + bound params.
  auto prepare = [](const char* cql) -> OpenSocialNet::Signaling::CassPreparedPtr
  {

    OpenSocialNet::Signaling::CassFuturePtr f { cass_session_prepare(gateway.cass_session.get(), cql) };
    if (cass_future_error_code(f.get()) != CASS_OK) scylla_die("scylla prepare", f.get());
    return OpenSocialNet::Signaling::CassPreparedPtr { cass_future_get_prepared(f.get()) };

  };

  gateway.prep_insert_message = prepare("INSERT INTO messages (channel_id, message_id, sender_id, content) VALUES (?, ?, ?, ?)");
  gateway.prep_fetch_history = prepare("SELECT message_id, sender_id, content FROM messages WHERE channel_id = ? AND message_id < ? LIMIT ?");
  gateway.prep_user_channels = prepare("SELECT channel_id FROM user_channels WHERE user_id = ?");

}

// Generate a TimeUUID client-side so we know the assigned id up front
// (and can echo it back to the sender in the broadcast event).
std::string scylla_insert_message(const std::string& channel_id, const std::string& sender_id, const std::string& content)
{

  CassUuid uuid { };
  cass_uuid_gen_time(gateway.uuid_gen.get(), &uuid);

  OpenSocialNet::Signaling::CassStatementPtr stmt { cass_prepared_bind(gateway.prep_insert_message.get()) };
  cass_statement_bind_string(stmt.get(), 0, channel_id.c_str());
  cass_statement_bind_uuid(stmt.get(), 1, uuid);
  cass_statement_bind_string(stmt.get(), 2, sender_id.c_str());
  cass_statement_bind_string(stmt.get(), 3, content.c_str());

  // TODO: this blocks the WS event loop. For real load, switch to
  // cass_future_set_callback() and resume the handler from the
  // driver's IO thread via uWS::Loop::defer().
  OpenSocialNet::Signaling::CassFuturePtr fut { cass_session_execute(gateway.cass_session.get(), stmt.get()) };
  cass_future_wait(fut.get());

  char uuid_str[CASS_UUID_STRING_LENGTH] { };
  cass_uuid_string(uuid, uuid_str);
  return uuid_str;

}

std::vector<std::string> scylla_user_channels(const std::string& user_id)
{

  std::vector<std::string> out { };

  OpenSocialNet::Signaling::CassStatementPtr stmt { cass_prepared_bind(gateway.prep_user_channels.get()) };
  cass_statement_bind_string(stmt.get(), 0, user_id.c_str());
  OpenSocialNet::Signaling::CassFuturePtr fut { cass_session_execute(gateway.cass_session.get(), stmt.get()) };
  cass_future_wait(fut.get());

  if (cass_future_error_code(fut.get()) == CASS_OK)
  {

    OpenSocialNet::Signaling::CassResultPtr result { cass_future_get_result(fut.get()) };
    OpenSocialNet::Signaling::CassIteratorPtr it { cass_iterator_from_result(result.get()) };
    while (cass_iterator_next(it.get()))
    {

      // row/value are borrowed from the iterator/result -- non-owning.
      const auto* row = cass_iterator_get_row(it.get());
      const char* s { nullptr };
      size_t len { 0 };
      cass_value_get_string(cass_row_get_column(row, 0), &s, &len);
      out.emplace_back(s, len);

    }

  }

  return out;

}


// ============================================================
// Kafka
// ============================================================

void kafka_init()
{

  std::string err { };
  std::unique_ptr<RdKafka::Conf> conf { RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL) };
  conf->set("bootstrap.servers", "localhost:9092", err);

  // Wrap immediately so there's no raw-pointer window even on the error path.
  gateway.producer.reset(RdKafka::Producer::create(conf.get(), err));
  if (!gateway.producer)
  {

    std::cerr << "kafka producer create failed: " << err << '\n';
    std::exit(1);

  }

}

// Runs on a dedicated thread. Pulls events from Kafka and re-publishes
// to local WS subscribers via uWS topics. This is what fans a message
// out to every gateway instance: each instance has its own consumer
// group (unique group.id), so each instance receives every event and
// publishes only to clients connected to itself.
void kafka_consumer_thread()
{

  std::string err { };
  std::unique_ptr<RdKafka::Conf> conf { RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL) };
  conf->set("bootstrap.servers", "localhost:9092", err);

  const auto now = std::chrono::system_clock::now().time_since_epoch().count();
  conf->set("group.id", "gateway-" + std::to_string(now), err);
  conf->set("auto.offset.reset", "latest", err);

  std::unique_ptr<RdKafka::KafkaConsumer> consumer { RdKafka::KafkaConsumer::create(conf.get(), err) };
  if (!consumer)
  {

    std::cerr << "kafka consumer create failed: " << err << '\n';
    return;

  }
  consumer->subscribe(std::vector { gateway.topic_name });

  while (gateway.running.load(std::memory_order_relaxed))
  {

    std::unique_ptr<RdKafka::Message> msg { consumer->consume(1000) };
    if (msg->err() == RdKafka::ERR__TIMED_OUT) continue;
    if (msg->err() != RdKafka::ERR_NO_ERROR)
    {

      std::cerr << "kafka consume error: " << msg->errstr() << '\n';
      continue;

    }

    // The Kafka payload is already a serialized Envelope containing
    // a ChatMessageEvent. Route it to the channel_id topic for fanout.
    signaling::Envelope envelope { };
    if (!envelope.ParseFromArray(msg->payload(), static_cast<int>(msg->len()))) continue;
    if (!envelope.has_chat_message_event()) continue;

    const auto& evt = envelope.chat_message_event();
    // uWS::App::publish is safe to call from any thread; it defers
    // the actual sends onto the WS loop.
    gateway.app->publish(evt.channel_id(), std::string_view { static_cast<const char*>(msg->payload()), msg->len() }, uWS::OpCode::BINARY);

  }

  consumer->close();

}


// ============================================================
// WS handlers -- all called from the uWS event loop thread
// ============================================================

void on_hello(WebSocket* ws, const signaling::Hello& hello)
{

  // sess is borrowed -- uWS owns the Session inside the WS handle.
  auto* sess = ws->getUserData();

  // TODO: verify hello.auth_token() against an auth service / sessions
  // table. For v1 we trust user_id.
  sess->user_id = hello.user_id();
  sess->session_id = make_session_id();
  sess->authenticated = true;
  std::cerr << "[hello] user=" << sess->user_id << " session=" << sess->session_id << '\n';

  signaling::Envelope envelope { };
  auto* ready = envelope.mutable_ready();
  ready->set_session_id(sess->session_id);

  // Subscribing the socket to each of the user's channels is how we
  // turn a single Kafka publish into N WS sends without keeping our
  // own (channel -> sockets) index. uWS owns that map for us.
  for (const auto& ch : scylla_user_channels(sess->user_id))
  {

    ws->subscribe(ch);
    ready->add_channel_ids(ch);

  }

  send_envelope(ws, envelope);

}

void on_send_message(WebSocket* ws, const signaling::SendMessage& req)
{

  auto* sess = ws->getUserData();
  if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }
  std::cerr << "[send] user=" << sess->user_id << " channel=" << req.channel_id() << " content=" << req.content() << '\n';

  // 1. Persist (source of truth, gives us canonical message_id).
  const std::string message_id { scylla_insert_message(req.channel_id(), sess->user_id, req.content()) };

  // 2. Build the broadcast envelope.
  signaling::Envelope out { };
  auto* evt = out.mutable_chat_message_event();
  evt->set_message_id(message_id);
  evt->set_client_nonce(req.client_nonce());
  evt->set_channel_id(req.channel_id());
  evt->set_sender_id(sess->user_id);
  evt->set_content(req.content());
  evt->set_timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

  std::string serialized { };
  out.SerializeToString(&serialized);

  // 3. Produce to Kafka. EVERY gateway's consumer (including this one)
  // will pick it up and call app->publish() locally.
  gateway.producer->produce(gateway.topic_name, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::MSG_COPY, serialized.data(), serialized.size(), req.channel_id().data(), req.channel_id().size(), 0, nullptr);
  gateway.producer->poll(0);

}

void on_fetch_history(WebSocket* ws, const signaling::FetchHistory& req)
{

  auto* sess = ws->getUserData();
  if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

  // Cursor: empty => the literal maximum v1 TimeUUID. We can't use
  // cass_uuid_max_from_time(LLONG_MAX, ...) here because the driver
  // internally multiplies (ms * 10000) to convert to 100ns intervals,
  // and LLONG_MAX * 10000 overflows uint64 -- wrapping to a value
  // smaller than real TimeUUIDs and excluding all rows.
  CassUuid cursor { };
  if (!req.before_message_id().empty()) cass_uuid_from_string(req.before_message_id().c_str(), &cursor);
  else cass_uuid_from_string("ffffffff-ffff-1fff-bfff-ffffffffffff", &cursor);

  const int32_t limit { (req.limit() == 0 || req.limit() > 100) ? 100 : static_cast<int32_t>(req.limit()) };

  OpenSocialNet::Signaling::CassStatementPtr stmt { cass_prepared_bind(gateway.prep_fetch_history.get()) };
  cass_statement_bind_string(stmt.get(), 0, req.channel_id().c_str());
  cass_statement_bind_uuid(stmt.get(), 1, cursor);
  cass_statement_bind_int32(stmt.get(), 2, limit);

  OpenSocialNet::Signaling::CassFuturePtr fut { cass_session_execute(gateway.cass_session.get(), stmt.get()) };
  cass_future_wait(fut.get());

  if (cass_future_error_code(fut.get()) != CASS_OK)
  {

    const char* err_msg { nullptr };
    size_t err_len { 0 };
    cass_future_error_message(fut.get(), &err_msg, &err_len);
    std::cerr << "[history] query failed: " << std::string_view { err_msg, err_len } << '\n';
    send_error(ws, 500, "history fetch failed");
    return;

  }

  signaling::Envelope envelope { };
  auto* resp = envelope.mutable_history_response();
  resp->set_request_id(req.request_id());

  OpenSocialNet::Signaling::CassResultPtr result { cass_future_get_result(fut.get()) };
  OpenSocialNet::Signaling::CassIteratorPtr it { cass_iterator_from_result(result.get()) };
  std::cerr << "[history] channel=" << req.channel_id() << " before=" << req.before_message_id() << " limit=" << limit << " row_count=" << cass_result_row_count(result.get()) << '\n';
  while (cass_iterator_next(it.get()))
  {

    const auto* row = cass_iterator_get_row(it.get());

    CassUuid uuid { };
    cass_value_get_uuid(cass_row_get_column(row, 0), &uuid);
    char uuid_str[CASS_UUID_STRING_LENGTH] { };
    cass_uuid_string(uuid, uuid_str);

    const char* sender { nullptr };
    size_t sender_len { 0 };
    cass_value_get_string(cass_row_get_column(row, 1), &sender, &sender_len);

    const char* content { nullptr };
    size_t content_len { 0 };
    cass_value_get_string(cass_row_get_column(row, 2), &content, &content_len);

    auto* evt = resp->add_msgs();
    evt->set_message_id(uuid_str);
    evt->set_channel_id(req.channel_id());
    evt->set_sender_id(std::string(sender, sender_len));
    evt->set_content(std::string(content, content_len));
    evt->set_timestamp_ms(cass_uuid_timestamp(uuid));

  }

  std::cerr << "[history] sending response with " << resp->msgs_size() << " msgs\n";
  send_envelope(ws, envelope);

}

void on_join_voice(WebSocket* ws, const signaling::JoinVoice& req)
{

  auto* sess = ws->getUserData();
  if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

  // Voice events get their own topic so they don't intermingle with
  // chat consumers on the same channel_id.
  ws->subscribe("voice:" + req.channel_id());

  // TODO: record this peer in an in-memory voice_peers[channel_id] map
  // (user_id, ip, port, ssrc). The ip/port comes from where the client
  // says its UDP socket is, sent via an extended JoinVoice -- or via
  // a STUN-style probe to the UDP receiver. Then publish VoicePeerJoined
  // to "voice:<channel>" so every existing peer learns about the joiner,
  // and reply directly to ws with one VoicePeerJoined per existing peer.

}

void on_leave_voice(WebSocket* ws, const signaling::LeaveVoice& req)
{

  ws->unsubscribe("voice:" + req.channel_id());
  // TODO: remove from voice_peers map, publish VoicePeerLeft.

}

// Top-level dispatcher: parse the frame, switch on which oneof case is set.
// This switch is literally the entire protocol surface area of the gateway.
void on_message(WebSocket* ws, std::string_view data, uWS::OpCode op)
{

  if (op != uWS::OpCode::BINARY) { send_error(ws, 400, "binary frames only"); return; }

  signaling::Envelope envelope { };
  if (!envelope.ParseFromArray(data.data(), static_cast<int>(data.size())))
  {

    send_error(ws, 400, "malformed envelope");
    return;

  }

  switch (envelope.payload_case())
  {

    case signaling::Envelope::kHello: on_hello(ws, envelope.hello()); break;
    case signaling::Envelope::kSendMessage: on_send_message(ws, envelope.send_message()); break;
    case signaling::Envelope::kFetchHistory: on_fetch_history(ws, envelope.fetch_history()); break;
    case signaling::Envelope::kJoinVoice: on_join_voice(ws, envelope.join_voice()); break;
    case signaling::Envelope::kLeaveVoice: on_leave_voice(ws, envelope.leave_voice()); break;

    case signaling::Envelope::kHeartbeat:
    {

      signaling::Envelope reply { };
      reply.mutable_heartbeat_ack()->set_nonce(envelope.heartbeat().nonce());
      send_envelope(ws, reply);
      break;

    }

    default:
      send_error(ws, 400, "unsupported payload");
      break;

  }

}

} // namespace


int main()
{

  // to verify protobuf library version compatibility; dont rmeove
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  scylla_init();
  kafka_init();

  std::thread consumer_thread { kafka_consumer_thread };

  uWS::App app { };
  gateway.app = &app;

  app.ws<Session>("/gateway",
  {
    .compression = uWS::DISABLED,
    .maxPayloadLength = 16 * 1024,
    .idleTimeout = 120,

    .open = [](WebSocket* ws)
    {

      // uWS zero-inits the Session for us; nothing to do until Hello.
      std::cout << "ws open\n";

    },

    .message = on_message,

    .close = [](WebSocket* ws, int code, std::string_view)
    {

      // uWS automatically unsubscribes the socket from all topics on
      // close, so we don't need to walk the subscribed list manually.
      std::cout << "ws close (" << code << ")\n";

    }
  });

  app.listen(9001, [](auto* token)
  {

    if (token) std::cout << "gateway listening on :9001\n";
    else std::cerr << "failed to listen on :9001\n";

  });

  app.run();

  // Shutdown
  gateway.running.store(false);
  consumer_thread.join();
  return 0;

}
