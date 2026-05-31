// related headers

// c sys headers
#include <cstdio>
#include <cstdlib>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <climits>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// 3rd party headers
#include <uwebsockets/App.h>
#include <cassandra.h>
#include <librdkafka/rdkafkacpp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>

// project headers
#include "CassandraDeleters.hh"
#include "SfuClient.hh"
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

  // Async Scylla bookkeeping. Driver callbacks fire on the driver's IO
  // thread; before touching a WebSocket we bounce back to the loop via
  // Loop::defer and look the socket up by its stable session id. Keying
  // by session_id (not the raw WS pointer) means we don't accidentally
  // match a recycled allocation: if a client closes and a brand new
  // connection grabs the same pointer before our defer drains, the new
  // connection has a different session_id and the lookup fails.
  // Populated in on_hello after auth succeeds, erased in .close.
  std::mutex live_sessions_mu { };
  std::unordered_map<std::string, WebSocket*> live_sessions { };

  // Auth. Read once at startup from OPENSOCIALNET_AUTH_SECRET. Empty
  // means "not configured" -- Hello frames are rejected in that case.
  std::string auth_secret { };

  // gRPC client to the SFU control plane. One stub for the process lifetime;
  // gRPC handles concurrency internally so it is shared across handlers.
  std::unique_ptr<OpenSocialNet::Signaling::SfuClient> sfu_client { };

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

// Returns the current WebSocket* for a session_id, or nullptr if the
// session is no longer registered. Must be called from the WS loop
// thread -- both .close (which erases) and any Loop::defer continuation
// (which calls this) run there, so reading inside that thread is
// race-free with respect to entries vanishing under our feet.
WebSocket* session_lookup(const std::string& session_id)
{

  if (session_id.empty()) return nullptr;
  std::lock_guard<std::mutex> lock { gateway.live_sessions_mu };
  auto it = gateway.live_sessions.find(session_id);
  return it != gateway.live_sessions.end() ? it->second : nullptr;

}

// scylla_user_channels stays synchronous: it runs exactly once per
// connection (in on_hello) and the result is needed before we can
// reply Ready, so there's nothing to overlap. Convert later if Hello
// turns into a hot path.
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
// Auth
// ============================================================

// HMAC-SHA256(secret, user_id) as lowercase hex. Output is always
// 64 chars; on OpenSSL failure we return an empty string so callers
// can distinguish that from a valid digest.
std::string compute_auth_token(const std::string& secret, const std::string& user_id)
{

  unsigned char mac[EVP_MAX_MD_SIZE] { };
  unsigned int mac_len { 0 };
  const auto* out = HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(user_id.data()), user_id.size(), mac, &mac_len);
  if (!out) return std::string { };

  std::string hex { };
  hex.resize(static_cast<size_t>(mac_len) * 2);
  static constexpr char digits[] { "0123456789abcdef" };
  for (unsigned int i { 0 }; i < mac_len; ++i)
  {

    hex[i * 2] = digits[(mac[i] >> 4) & 0xF];
    hex[i * 2 + 1] = digits[mac[i] & 0xF];

  }
  return hex;

}


// ============================================================
// WS handlers -- all called from the uWS event loop thread
// ============================================================

void on_hello(WebSocket* ws, const signaling::Hello& hello)
{

  // sess is borrowed -- uWS owns the Session inside the WS handle.
  auto* sess = ws->getUserData();

  if (gateway.auth_secret.empty())
  {

    send_error(ws, 401, "auth not configured");
    ws->close();
    return;

  }

  if (hello.user_id().empty())
  {

    send_error(ws, 400, "missing user_id");
    ws->close();
    return;

  }

  // Constant-time compare so a network attacker can't time-leak which
  // prefix of the expected token they got right.
  const std::string expected { compute_auth_token(gateway.auth_secret, hello.user_id()) };
  const std::string& got { hello.auth_token() };
  const bool ok { !expected.empty() && expected.size() == got.size() && CRYPTO_memcmp(expected.data(), got.data(), expected.size()) == 0 };
  if (!ok)
  {

    std::cerr << "[auth] denied user=" << hello.user_id() << '\n';
    send_error(ws, 401, "invalid auth token");
    ws->close();
    return;

  }

  sess->user_id = hello.user_id();
  sess->session_id = make_session_id();
  sess->authenticated = true;

  // Register the session AFTER auth: async continuations look the socket
  // up by session_id, so pre-auth connections are deliberately invisible.
  {

    std::lock_guard<std::mutex> lock { gateway.live_sessions_mu };
    gateway.live_sessions[sess->session_id] = ws;

  }

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

// Per-INSERT continuation state. Heap-allocated so it outlives the
// scope of on_send_message: the driver owns the lifetime via the
// raw pointer handed to cass_future_set_callback's user_data, and
// the callback unique_ptr's it back to free everything on completion.
struct InsertCtx
{

  std::string session_id { }; // stable id of the requesting connection
  uWS::Loop* loop { nullptr };
  OpenSocialNet::Signaling::CassFuturePtr fut { };
  std::string message_id { };
  std::string channel_id { };
  std::string sender_id { };
  std::string content { };
  std::string client_nonce { };

};

void on_insert_complete(CassFuture* /*future*/, void* data);

void on_send_message(WebSocket* ws, const signaling::SendMessage& req)
{

  auto* sess = ws->getUserData();
  if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }
  std::cerr << "[send] user=" << sess->user_id << " channel=" << req.channel_id() << " content=" << req.content() << '\n';

  // Generate the TimeUUID client-side so we know the assigned id
  // before the INSERT even runs -- the Kafka fanout in the callback
  // needs to embed it in the broadcast event.
  CassUuid uuid { };
  cass_uuid_gen_time(gateway.uuid_gen.get(), &uuid);
  char uuid_str[CASS_UUID_STRING_LENGTH] { };
  cass_uuid_string(uuid, uuid_str);

  OpenSocialNet::Signaling::CassStatementPtr stmt { cass_prepared_bind(gateway.prep_insert_message.get()) };
  cass_statement_bind_string(stmt.get(), 0, req.channel_id().c_str());
  cass_statement_bind_uuid(stmt.get(), 1, uuid);
  cass_statement_bind_string(stmt.get(), 2, sess->user_id.c_str());
  cass_statement_bind_string(stmt.get(), 3, req.content().c_str());

  // Copy everything we'll need in the callback BEFORE returning -- by
  // the time the driver IO thread fires us, `req` and `sess` are gone.
  auto ctx = std::make_unique<InsertCtx>();
  ctx->session_id = sess->session_id;
  ctx->loop = uWS::Loop::get();
  ctx->fut.reset(cass_session_execute(gateway.cass_session.get(), stmt.get()));
  ctx->message_id = uuid_str;
  ctx->channel_id = req.channel_id();
  ctx->sender_id = sess->user_id;
  ctx->content = req.content();
  ctx->client_nonce = req.client_nonce();

  cass_future_set_callback(ctx->fut.get(), &on_insert_complete, ctx.get());
  ctx.release();

}

void on_insert_complete(CassFuture* /*future*/, void* data)
{

  // Runs on the driver's IO thread. Take ownership immediately so the
  // context (and the CassFuture inside it) is freed even if defer throws.
  std::unique_ptr<InsertCtx> ctx { static_cast<InsertCtx*>(data) };

  if (cass_future_error_code(ctx->fut.get()) != CASS_OK)
  {

    const char* err_msg { nullptr };
    size_t err_len { 0 };
    cass_future_error_message(ctx->fut.get(), &err_msg, &err_len);
    std::cerr << "[send] insert failed: " << std::string_view { err_msg, err_len } << '\n';
    // Don't fan out a message we didn't persist; just drop. The client
    // will time out its pending nonce and retry.
    return;

  }

  // Bounce back onto the WS loop before doing anything WS- or Kafka-
  // related. We move the ctx into the deferred lambda so it survives
  // until the loop drains it.
  InsertCtx* raw { ctx.release() };
  raw->loop->defer([raw]()
  {

    std::unique_ptr<InsertCtx> c { raw };

    signaling::Envelope out { };
    auto* evt = out.mutable_chat_message_event();
    evt->set_message_id(c->message_id);
    evt->set_client_nonce(c->client_nonce);
    evt->set_channel_id(c->channel_id);
    evt->set_sender_id(c->sender_id);
    evt->set_content(c->content);
    evt->set_timestamp_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

    std::string serialized { };
    out.SerializeToString(&serialized);

    // Direct echo to the sender (Discord-style instant confirmation) -- only
    // if the session is still around. Once subscriptions for self-channels
    // are wired up, the Kafka fanout will also need to skip the sender to
    // avoid double-delivery. Today user_channels is empty so that isn't an
    // issue.
    if (WebSocket* ws = session_lookup(c->session_id); ws != nullptr) ws->send(serialized, uWS::OpCode::BINARY);

    // Produce to Kafka regardless of WS liveness -- the message is
    // persisted, every other subscriber on every gateway still needs
    // it. librdkafka's produce is buffered, so this is non-blocking.
    gateway.producer->produce(gateway.topic_name, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::MSG_COPY, serialized.data(), serialized.size(), c->channel_id.data(), c->channel_id.size(), 0, nullptr);
    gateway.producer->poll(0);

  });

}

// Per-history-fetch continuation state. Same lifetime trick as InsertCtx:
// heap-allocated, raw ptr ridden through cass_future_set_callback's
// user_data slot, freed by the unique_ptr inside the callback.
struct HistoryCtx
{

  std::string session_id { }; // stable id of the requesting connection
  uWS::Loop* loop { nullptr };
  OpenSocialNet::Signaling::CassFuturePtr fut { };
  std::string request_id { };
  std::string channel_id { };
  std::string before_message_id { };
  int32_t limit { 0 };

};

void on_history_complete(CassFuture* /*future*/, void* data);

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

  auto ctx = std::make_unique<HistoryCtx>();
  ctx->session_id = sess->session_id;
  ctx->loop = uWS::Loop::get();
  ctx->fut.reset(cass_session_execute(gateway.cass_session.get(), stmt.get()));
  ctx->request_id = req.request_id();
  ctx->channel_id = req.channel_id();
  ctx->before_message_id = req.before_message_id();
  ctx->limit = limit;

  cass_future_set_callback(ctx->fut.get(), &on_history_complete, ctx.get());
  ctx.release();

}

void on_history_complete(CassFuture* /*future*/, void* data)
{

  // On the driver's IO thread. Take ownership of the ctx but DON'T
  // touch the WebSocket -- that has to happen on the loop thread.
  HistoryCtx* raw { static_cast<HistoryCtx*>(data) };

  raw->loop->defer([raw]()
  {

    std::unique_ptr<HistoryCtx> c { raw };

    // Client could have disconnected while the query was in flight.
    // Lookup by session_id inside the loop thread is race-free because
    // both .close (which erases) and this defer run on the same thread.
    WebSocket* ws = session_lookup(c->session_id);
    if (ws == nullptr)
    {

      std::cerr << "[history] session gone, dropping response\n";
      return;

    }

    if (cass_future_error_code(c->fut.get()) != CASS_OK)
    {

      const char* err_msg { nullptr };
      size_t err_len { 0 };
      cass_future_error_message(c->fut.get(), &err_msg, &err_len);
      std::cerr << "[history] query failed: " << std::string_view { err_msg, err_len } << '\n';
      send_error(ws, 500, "history fetch failed");
      return;

    }

    signaling::Envelope envelope { };
    auto* resp = envelope.mutable_history_response();
    resp->set_request_id(c->request_id);

    OpenSocialNet::Signaling::CassResultPtr result { cass_future_get_result(c->fut.get()) };
    OpenSocialNet::Signaling::CassIteratorPtr it { cass_iterator_from_result(result.get()) };
    std::cerr << "[history] channel=" << c->channel_id << " before=" << c->before_message_id << " limit=" << c->limit << " row_count=" << cass_result_row_count(result.get()) << '\n';
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
      evt->set_channel_id(c->channel_id);
      evt->set_sender_id(std::string(sender, sender_len));
      evt->set_content(std::string(content, content_len));
      evt->set_timestamp_ms(cass_uuid_timestamp(uuid));

    }

    std::cerr << "[history] sending response with " << resp->msgs_size() << " msgs\n";
    send_envelope(ws, envelope);

  });

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

// Derive the SFU-side peer_id for a connection. The SFU treats (room_id,
// peer_id) as the routing key; we combine the user_id with the per-connection
// session_id so two tabs from the same user end up as distinct peers.
std::string make_peer_id(const Session& sess)
{

  return sess.user_id + ":" + sess.session_id;

}

// Browser pushed an SDP offer. Forward it to the SFU and relay the answer
// back on the same WS. Synchronous call: AddPeer is one-shot per browser
// and not on the chat hot path.
void on_sdp_offer(WebSocket* ws, const signaling::Sdp& offer)
{

  auto* sess = ws->getUserData();
  if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }
  if (gateway.sfu_client == nullptr) { send_error(ws, 503, "sfu unavailable"); return; }

  const std::string peer_id { make_peer_id(*sess) };
  signaling::Sdp answer { };
  std::string err { };
  if (!gateway.sfu_client->add_peer(offer.room_id(), peer_id, offer, &answer, &err))
  {

    std::cerr << "[sfu] AddPeer failed: " << err << '\n';
    send_error(ws, 502, "sfu add_peer failed");
    return;

  }

  signaling::Envelope envelope { };
  *envelope.mutable_sdp_answer() = answer;
  send_envelope(ws, envelope);

}

// Browser trickled a local ICE candidate. Fire-and-forget into the SFU.
void on_ice_candidate(WebSocket* ws, const signaling::IceCandidate& candidate)
{

  auto* sess = ws->getUserData();
  if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }
  if (gateway.sfu_client == nullptr) return;

  const std::string peer_id { make_peer_id(*sess) };
  gateway.sfu_client->add_remote_ice_candidate(candidate.room_id(), peer_id, candidate);

}

// Dispatched off the SFU event-stream reader thread. We bounce the WS write
// back to the loop thread (uWS requires it) via Loop::defer, the same pattern
// the Scylla async continuations use. Look up the socket by session_id inside
// the deferred lambda so a disconnect mid-event is race-free.
void on_sfu_peer_event(const ::sfu_control::PeerEvent& event)
{

  // The peer_id encoding is "user_id:session_id" -- extract the session id
  // suffix so we can find the WS in live_sessions.
  const std::string& peer_id { event.peer_id() };
  const auto colon { peer_id.rfind(':') };
  if (colon == std::string::npos) { std::cerr << "[sfu] malformed peer_id: " << peer_id << '\n'; return; }
  std::string session_id { peer_id.substr(colon + 1) };
  std::string room_id { event.room_id() };

  auto* loop = uWS::Loop::get();
  if (loop == nullptr) return;

  if (event.has_local_ice_candidate())
  {

    signaling::IceCandidate ice { event.local_ice_candidate() };
    loop->defer([session_id = std::move(session_id), ice = std::move(ice)]() mutable
    {

      WebSocket* ws { session_lookup(session_id) };
      if (ws == nullptr) return;
      signaling::Envelope envelope { };
      *envelope.mutable_server_ice_candidate() = std::move(ice);
      send_envelope(ws, envelope);

    });
    return;

  }

  if (event.has_peer_ready() && event.peer_ready())
  {

    loop->defer([session_id = std::move(session_id), room_id = std::move(room_id)]()
    {

      WebSocket* ws { session_lookup(session_id) };
      if (ws == nullptr) return;
      signaling::Envelope envelope { };
      envelope.mutable_peer_ready()->set_room_id(room_id);
      send_envelope(ws, envelope);

    });

  }

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
    case signaling::Envelope::kSdpOffer: on_sdp_offer(ws, envelope.sdp_offer()); break;
    case signaling::Envelope::kIceCandidate: on_ice_candidate(ws, envelope.ice_candidate()); break;

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

  // Read once: every Hello handler reads gateway.auth_secret directly,
  // and we never reload at runtime.
  if (const char* secret = std::getenv("OPENSOCIALNET_AUTH_SECRET"); secret && *secret)
  {

    gateway.auth_secret = secret;

  }
  else
  {

    std::cerr << "[auth] WARNING: OPENSOCIALNET_AUTH_SECRET is unset; all Hello frames will be rejected\n";

  }

  scylla_init();
  kafka_init();

  // SFU gRPC control-plane client. One stub for the process lifetime; gRPC
  // handles concurrency. The event stream runs on its own background thread
  // and bounces WS writes back onto the uWS loop via Loop::defer.
  gateway.sfu_client = std::make_unique<OpenSocialNet::Signaling::SfuClient>("127.0.0.1:50051");
  gateway.sfu_client->start_event_stream(&on_sfu_peer_event);

  std::thread consumer_thread { kafka_consumer_thread };

  uWS::App app { };
  gateway.app = &app;

  app.ws<Session>("/gateway",
  {
    .compression = uWS::DISABLED,
    .maxPayloadLength = 16 * 1024,
    .idleTimeout = 120,

    .open = [](WebSocket* /*ws*/)
    {

      // Session isn't registered yet -- we don't have a session_id until
      // Hello succeeds. Pre-auth connections can't be the target of async
      // continuations anyway, so being invisible to session_lookup is fine.
      std::cout << "ws open\n";

    },

    .message = on_message,

    .close = [](WebSocket* ws, int code, std::string_view)
    {

      auto* sess = ws->getUserData();
      if (!sess->session_id.empty())
      {

        std::lock_guard<std::mutex> lock { gateway.live_sessions_mu };
        gateway.live_sessions.erase(sess->session_id);

      }
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
  if (gateway.sfu_client) gateway.sfu_client->shutdown();
  consumer_thread.join();
  return 0;

}
