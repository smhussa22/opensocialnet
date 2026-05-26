// related headers

// c sys headers
#include <cstdio>

// cpp stdlib headers
#include <atomic>
#include <chrono>
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

  std::string user_id;
  std::string session_id;
  bool        authenticated { false };

};

// uWS template params: <SSL, isServer, UserData>
using WS = uWS::WebSocket<false, true, Session>;


// ============================================================
// Process-wide state. One gateway instance owns one of these.
// ============================================================

struct Gateway
{

  uWS::App* app { nullptr };

  // Scylla
  CassCluster*        cass_cluster        { nullptr };
  CassSession*        cass_session        { nullptr };
  CassUuidGen*        uuid_gen            { nullptr };
  const CassPrepared* prep_insert_message { nullptr };
  const CassPrepared* prep_fetch_history  { nullptr };
  const CassPrepared* prep_user_channels  { nullptr };

  // Kafka
  std::unique_ptr<RdKafka::Producer> producer;
  std::string topic_name { "chat_events" };

  // Lifecycle
  std::atomic<bool> running { true };

};

Gateway g;


// ============================================================
// Generic helpers
// ============================================================

void send_envelope(WS* ws, const signaling::Envelope& env)
{

  std::string buf;
  env.SerializeToString(&buf);
  ws->send(buf, uWS::OpCode::BINARY);

}

void send_error(WS* ws, uint32_t code, std::string_view msg)
{

  signaling::Envelope env;
  auto* e = env.mutable_error();
  e->set_code(code);
  e->set_message(std::string(msg));
  send_envelope(ws, env);

}

std::string make_session_id()
{

  // 16 hex chars from a thread-local PRNG. Real impl: UUIDv4 from CSPRNG.
  thread_local std::mt19937_64 rng { std::random_device{}() };
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(rng()));
  return std::string(buf, 16);

}


// ============================================================
// Scylla
// ============================================================

[[noreturn]] void scylla_die(const char* what, CassFuture* f)
{

  const char* msg; size_t len;
  cass_future_error_message(f, &msg, &len);
  std::cerr << what << " failed: " << std::string(msg, len) << '\n';
  std::exit(1);

}

void scylla_init()
{

  g.cass_cluster = cass_cluster_new();
  cass_cluster_set_contact_points(g.cass_cluster, "127.0.0.1");

  g.cass_session = cass_session_new();
  g.uuid_gen     = cass_uuid_gen_new();

  auto* fut = cass_session_connect_keyspace(g.cass_session, g.cass_cluster, "opensocialnet");
  if (cass_future_error_code(fut) != CASS_OK) scylla_die("scylla connect", fut);
  cass_future_free(fut);

  // Prepared statements: parsed once on Scylla, cached, reused per call.
  // Server keeps a md5 -> AST map; we just send the hash + bound params.
  auto prepare = [](const char* cql) -> const CassPrepared*
  {

    auto* f = cass_session_prepare(g.cass_session, cql);
    if (cass_future_error_code(f) != CASS_OK) scylla_die("scylla prepare", f);
    const auto* p = cass_future_get_prepared(f);
    cass_future_free(f);
    return p;

  };

  g.prep_insert_message = prepare(
    "INSERT INTO messages (channel_id, message_id, sender_id, content) "
    "VALUES (?, ?, ?, ?)");

  g.prep_fetch_history = prepare(
    "SELECT message_id, sender_id, content FROM messages "
    "WHERE channel_id = ? AND message_id < ? LIMIT ?");

  g.prep_user_channels = prepare(
    "SELECT channel_id FROM user_channels WHERE user_id = ?");

}

// Generate a TimeUUID client-side so we know the assigned id up front
// (and can echo it back to the sender in the broadcast event).
std::string scylla_insert_message(const std::string& channel_id,
                                  const std::string& sender_id,
                                  const std::string& content)
{

  CassUuid uuid;
  cass_uuid_gen_time(g.uuid_gen, &uuid);

  auto* stmt = cass_prepared_bind(g.prep_insert_message);
  cass_statement_bind_string(stmt, 0, channel_id.c_str());
  cass_statement_bind_uuid  (stmt, 1, uuid);
  cass_statement_bind_string(stmt, 2, sender_id.c_str());
  cass_statement_bind_string(stmt, 3, content.c_str());

  // TODO: this blocks the WS event loop. For real load, switch to
  // cass_future_set_callback() and resume the handler from the
  // driver's IO thread via uWS::Loop::defer().
  auto* fut = cass_session_execute(g.cass_session, stmt);
  cass_future_wait(fut);
  cass_statement_free(stmt);
  cass_future_free(fut);

  char uuid_str[CASS_UUID_STRING_LENGTH];
  cass_uuid_string(uuid, uuid_str);
  return uuid_str;

}

std::vector<std::string> scylla_user_channels(const std::string& user_id)
{

  std::vector<std::string> out;

  auto* stmt = cass_prepared_bind(g.prep_user_channels);
  cass_statement_bind_string(stmt, 0, user_id.c_str());
  auto* fut = cass_session_execute(g.cass_session, stmt);
  cass_future_wait(fut);

  if (cass_future_error_code(fut) == CASS_OK)
  {

    const auto* result = cass_future_get_result(fut);
    auto* it = cass_iterator_from_result(result);
    while (cass_iterator_next(it))
    {

      const auto* row = cass_iterator_get_row(it);
      const char* s; size_t len;
      cass_value_get_string(cass_row_get_column(row, 0), &s, &len);
      out.emplace_back(s, len);

    }
    cass_iterator_free(it);
    cass_result_free(const_cast<CassResult*>(result));

  }

  cass_future_free(fut);
  cass_statement_free(stmt);
  return out;

}


// ============================================================
// Kafka
// ============================================================

void kafka_init()
{

  std::string err;
  std::unique_ptr<RdKafka::Conf> conf { RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL) };
  conf->set("bootstrap.servers", "localhost:9092", err);

  auto* raw = RdKafka::Producer::create(conf.get(), err);
  if (!raw)
  {

    std::cerr << "kafka producer create failed: " << err << '\n';
    std::exit(1);

  }
  g.producer.reset(raw);

}

// Runs on a dedicated thread. Pulls events from Kafka and re-publishes
// to local WS subscribers via uWS topics. This is what fans a message
// out to every gateway instance: each instance has its own consumer
// group (unique group.id), so each instance receives every event and
// publishes only to clients connected to itself.
void kafka_consumer_thread()
{

  std::string err;
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
  consumer->subscribe(std::vector{ g.topic_name });

  while (g.running.load(std::memory_order_relaxed))
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
    signaling::Envelope env;
    if (!env.ParseFromArray(msg->payload(), static_cast<int>(msg->len()))) continue;
    if (!env.has_chat_message_event()) continue;

    const auto& evt = env.chat_message_event();
    // uWS::App::publish is safe to call from any thread; it defers
    // the actual sends onto the WS loop.
    g.app->publish(evt.channel_id(),
                   std::string_view{ static_cast<const char*>(msg->payload()), msg->len() },
                   uWS::OpCode::BINARY);

  }

  consumer->close();

}


// ============================================================
// WS handlers -- all called from the uWS event loop thread
// ============================================================

void on_hello(WS* ws, const signaling::Hello& hello)
{

  auto* sess = ws->getUserData();

  // TODO: verify hello.auth_token() against an auth service / sessions
  // table. For v1 we trust user_id.
  sess->user_id       = hello.user_id();
  sess->session_id    = make_session_id();
  sess->authenticated = true;

  signaling::Envelope env;
  auto* ready = env.mutable_ready();
  ready->set_session_id(sess->session_id);

  // Subscribing the socket to each of the user's channels is how we
  // turn a single Kafka publish into N WS sends without keeping our
  // own (channel -> sockets) index. uWS owns that map for us.
  for (const auto& ch : scylla_user_channels(sess->user_id))
  {

    ws->subscribe(ch);
    ready->add_channel_ids(ch);

  }

  send_envelope(ws, env);

}

void on_send_message(WS* ws, const signaling::SendMessage& req)
{

  auto* sess = ws->getUserData();
  if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

  // 1. Persist (source of truth, gives us canonical message_id).
  const std::string message_id =
    scylla_insert_message(req.channel_id(), sess->user_id, req.content());

  // 2. Build the broadcast envelope.
  signaling::Envelope out;
  auto* evt = out.mutable_chat_message_event();
  evt->set_message_id  (message_id);
  evt->set_client_nonce(req.client_nonce());
  evt->set_channel_id  (req.channel_id());
  evt->set_sender_id   (sess->user_id);
  evt->set_content     (req.content());
  evt->set_timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count());

  std::string serialized;
  out.SerializeToString(&serialized);

  // 3. Produce to Kafka. EVERY gateway's consumer (including this one)
  // will pick it up and call app->publish() locally.
  g.producer->produce(g.topic_name,
                       RdKafka::Topic::PARTITION_UA,
                       RdKafka::Producer::MSG_COPY,
                       serialized.data(), serialized.size(),
                       req.channel_id().data(), req.channel_id().size(),
                       0, nullptr);
  g.producer->poll(0);

}

void on_fetch_history(WS* ws, const signaling::FetchHistory& req)
{

  auto* sess = ws->getUserData();
  if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

  // TODO: bind req.channel_id() + req.before_message_id() (as TimeUUID;
  // use maxTimeuuid(now) if empty) + req.limit(), run prep_fetch_history,
  // iterate rows, populate HistoryResponse.msgs[].
  signaling::Envelope env;
  env.mutable_history_response()->set_request_id(req.request_id());
  send_envelope(ws, env);

}

void on_join_voice(WS* ws, const signaling::JoinVoice& req)
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

void on_leave_voice(WS* ws, const signaling::LeaveVoice& req)
{

  ws->unsubscribe("voice:" + req.channel_id());
  // TODO: remove from voice_peers map, publish VoicePeerLeft.

}

// Top-level dispatcher: parse the frame, switch on which oneof case is set.
// This switch is literally the entire protocol surface area of the gateway.
void on_message(WS* ws, std::string_view data, uWS::OpCode op)
{

  if (op != uWS::OpCode::BINARY) { send_error(ws, 400, "binary frames only"); return; }

  signaling::Envelope env;
  if (!env.ParseFromArray(data.data(), static_cast<int>(data.size())))
  {

    send_error(ws, 400, "malformed envelope");
    return;

  }

  using P = signaling::Envelope;
  switch (env.payload_case())
  {

    case P::kHello:        on_hello        (ws, env.hello());          break;
    case P::kSendMessage:  on_send_message (ws, env.send_message());   break;
    case P::kFetchHistory: on_fetch_history(ws, env.fetch_history());  break;
    case P::kJoinVoice:    on_join_voice   (ws, env.join_voice());     break;
    case P::kLeaveVoice:   on_leave_voice  (ws, env.leave_voice());    break;

    case P::kHeartbeat:
    {

      signaling::Envelope reply;
      reply.mutable_heartbeat_ack()->set_nonce(env.heartbeat().nonce());
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

  GOOGLE_PROTOBUF_VERIFY_VERSION;

  scylla_init();
  kafka_init();

  std::thread consumer_thread { kafka_consumer_thread };

  uWS::App app;
  g.app = &app;

  app.ws<Session>("/gateway",
  {
    .compression      = uWS::DISABLED,
    .maxPayloadLength = 16 * 1024,
    .idleTimeout      = 120,

    .open = [](WS* ws)
    {

      // uWS zero-inits the Session for us; nothing to do until Hello.
      std::cout << "ws open\n";

    },

    .message = on_message,

    .close = [](WS* ws, int code, std::string_view)
    {

      // uWS automatically unsubscribes the socket from all topics on
      // close, so we don't need to walk the subscribed list manually.
      std::cout << "ws close (" << code << ")\n";

    }
  });

  app.listen(9001, [](auto* token)
  {

    if (token) std::cout << "gateway listening on :9001\n";
    else       std::cerr << "failed to listen on :9001\n";

  });

  app.run();

  // Shutdown
  g.running.store(false);
  consumer_thread.join();
  return 0;

}
