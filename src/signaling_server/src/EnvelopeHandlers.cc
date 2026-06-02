// related headers
#include "EnvelopeHandlers.hh"

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// 3rd party headers
#include <uwebsockets/App.h>
#include <cassandra.h>
#include <librdkafka/rdkafkacpp.h>

// project headers
#include "Auth.hh"
#include "CassandraDeleters.hh"
#include "GatewayState.hh"
#include "KafkaBus.hh"
#include "ScyllaClient.hh"
#include "Session.hh"
#include "SfuClient.hh"


namespace OpenSocialNet::Signaling
{

    // ============================================================
    // Generic helpers
    // ============================================================

    void send_envelope(WebSocket* ws, const ::signaling::Envelope& envelope)
    {

        std::string buf { };
        envelope.SerializeToString(&buf);
        ws->send(buf, ::uWS::OpCode::BINARY);

    }

    void send_error(WebSocket* ws, std::uint32_t code, std::string_view msg)
    {

        ::signaling::Envelope envelope { };
        // env.mutable_error() returns a non-owning ptr -- the Envelope owns it.
        auto* e = envelope.mutable_error();
        e->set_code(code);
        e->set_message(std::string(msg));
        send_envelope(ws, envelope);

    }


    namespace
    {

        // Per-INSERT continuation state. Heap-allocated so it outlives the
        // scope of on_send_message: the driver owns the lifetime via the
        // raw pointer handed to cass_future_set_callback's user_data, and
        // the callback unique_ptr's it back to free everything on completion.
        struct InsertCtx
        {

            std::string session_id { }; // stable id of the requesting connection
            ::uWS::Loop* loop { nullptr }; // WS loop we bounce back onto
            GatewayState* state { nullptr }; // shared deps so the defer can find them
            CassFuturePtr fut { }; // INSERT future, owns the driver handle
            std::string message_id { }; // generated TimeUUID for the new row
            std::string channel_id { }; // target channel for fanout
            std::string sender_id { }; // who sent it
            std::string content { }; // body
            std::string client_nonce { }; // echoed back so the client can ack

        };

        // Per-history-fetch continuation state. Same lifetime trick as
        // InsertCtx: heap-allocated, raw ptr ridden through
        // cass_future_set_callback's user_data slot, freed by the unique_ptr
        // inside the callback.
        struct HistoryCtx
        {

            std::string session_id { }; // stable id of the requesting connection
            ::uWS::Loop* loop { nullptr }; // WS loop we bounce back onto
            GatewayState* state { nullptr }; // shared deps so the defer can find them
            CassFuturePtr fut { }; // SELECT future, owns the driver handle
            std::string request_id { }; // echoed back so the client can correlate
            std::string channel_id { }; // channel we are paginating in
            std::string before_message_id { }; // cursor: send rows older than this id
            std::int32_t limit { 0 }; // clamped row count to return

        };

        void on_insert_complete(::CassFuture* /*future*/, void* data);
        void on_history_complete(::CassFuture* /*future*/, void* data);

        // Derive the SFU-side peer_id for a connection. The SFU treats
        // (room_id, peer_id) as the routing key; we combine the user_id
        // with the per-connection session_id so two tabs from the same
        // user end up as distinct peers.
        std::string make_peer_id(const Session& sess)
        {

            return sess.user_id + ":" + sess.session_id;

        }

        void on_insert_complete(::CassFuture* /*future*/, void* data)
        {

            // Runs on the driver's IO thread. Take ownership immediately so the
            // context (and the CassFuture inside it) is freed even if defer throws.
            std::unique_ptr<InsertCtx> ctx { static_cast<InsertCtx*>(data) };

            if (::cass_future_error_code(ctx->fut.get()) != CASS_OK)
            {

                const char* err_msg { nullptr };
                std::size_t err_len { 0 };
                ::cass_future_error_message(ctx->fut.get(), &err_msg, &err_len);
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

                ::signaling::Envelope out { };
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
                if (WebSocket* ws = c->state->sessions->lookup(c->session_id); ws != nullptr) ws->send(serialized, ::uWS::OpCode::BINARY);

                // Produce to Kafka regardless of WS liveness -- the message is
                // persisted, every other subscriber on every gateway still needs
                // it. librdkafka's produce is buffered, so this is non-blocking.
                auto* producer = c->state->kafka->producer();
                const std::string& topic_name = c->state->kafka->topic_name();
                producer->produce(topic_name, ::RdKafka::Topic::PARTITION_UA, ::RdKafka::Producer::MSG_COPY, serialized.data(), serialized.size(), c->channel_id.data(), c->channel_id.size(), 0, nullptr);
                producer->poll(0);

            });

        }

        void on_history_complete(::CassFuture* /*future*/, void* data)
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
                WebSocket* ws = c->state->sessions->lookup(c->session_id);
                if (ws == nullptr)
                {

                    std::cerr << "[history] session gone, dropping response\n";
                    return;

                }

                if (::cass_future_error_code(c->fut.get()) != CASS_OK)
                {

                    const char* err_msg { nullptr };
                    std::size_t err_len { 0 };
                    ::cass_future_error_message(c->fut.get(), &err_msg, &err_len);
                    std::cerr << "[history] query failed: " << std::string_view { err_msg, err_len } << '\n';
                    send_error(ws, 500, "history fetch failed");
                    return;

                }

                ::signaling::Envelope envelope { };
                auto* resp = envelope.mutable_history_response();
                resp->set_request_id(c->request_id);

                CassResultPtr result { ::cass_future_get_result(c->fut.get()) };
                CassIteratorPtr it { ::cass_iterator_from_result(result.get()) };
                std::cerr << "[history] channel=" << c->channel_id << " before=" << c->before_message_id << " limit=" << c->limit << " row_count=" << ::cass_result_row_count(result.get()) << '\n';
                while (::cass_iterator_next(it.get()))
                {

                    const auto* row = ::cass_iterator_get_row(it.get());

                    ::CassUuid uuid { };
                    ::cass_value_get_uuid(::cass_row_get_column(row, 0), &uuid);
                    char uuid_str[CASS_UUID_STRING_LENGTH] { };
                    ::cass_uuid_string(uuid, uuid_str);

                    const char* sender { nullptr };
                    std::size_t sender_len { 0 };
                    ::cass_value_get_string(::cass_row_get_column(row, 1), &sender, &sender_len);

                    const char* content { nullptr };
                    std::size_t content_len { 0 };
                    ::cass_value_get_string(::cass_row_get_column(row, 2), &content, &content_len);

                    auto* evt = resp->add_msgs();
                    evt->set_message_id(uuid_str);
                    evt->set_channel_id(c->channel_id);
                    evt->set_sender_id(std::string(sender, sender_len));
                    evt->set_content(std::string(content, content_len));
                    evt->set_timestamp_ms(::cass_uuid_timestamp(uuid));

                }

                std::cerr << "[history] sending response with " << resp->msgs_size() << " msgs\n";
                send_envelope(ws, envelope);

            });

        }

    }


    // ============================================================
    // WS handlers -- all called from the uWS event loop thread
    // ============================================================

    void on_hello(GatewayState& state, WebSocket* ws, const ::signaling::Hello& hello)
    {

        // sess is borrowed -- uWS owns the Session inside the WS handle.
        auto* sess = ws->getUserData();

        if (state.auth_secret.empty())
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

        if (!verify_hello_auth(state.auth_secret, hello.user_id(), hello.auth_token()))
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
        state.sessions->add(sess->session_id, ws);

        std::cerr << "[hello] user=" << sess->user_id << " session=" << sess->session_id << '\n';

        ::signaling::Envelope envelope { };
        auto* ready = envelope.mutable_ready();
        ready->set_session_id(sess->session_id);

        // Subscribing the socket to each of the user's channels is how we
        // turn a single Kafka publish into N WS sends without keeping our
        // own (channel -> sockets) index. uWS owns that map for us.
        for (const auto& ch : state.scylla->user_channels(sess->user_id))
        {

            ws->subscribe(ch);
            ready->add_channel_ids(ch);

        }

        send_envelope(ws, envelope);

    }

    void on_heartbeat(WebSocket* ws, const ::signaling::Heartbeat& heartbeat)
    {

        ::signaling::Envelope reply { };
        reply.mutable_heartbeat_ack()->set_nonce(heartbeat.nonce());
        send_envelope(ws, reply);

    }

    void on_send_message(GatewayState& state, WebSocket* ws, const ::signaling::SendMessage& req)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }
        std::cerr << "[send] user=" << sess->user_id << " channel=" << req.channel_id() << " content=" << req.content() << '\n';

        // Generate the TimeUUID client-side so we know the assigned id
        // before the INSERT even runs -- the Kafka fanout in the callback
        // needs to embed it in the broadcast event.
        ::CassUuid uuid { };
        ::cass_uuid_gen_time(state.scylla->uuid_gen(), &uuid);
        char uuid_str[CASS_UUID_STRING_LENGTH] { };
        ::cass_uuid_string(uuid, uuid_str);

        CassStatementPtr stmt { ::cass_prepared_bind(state.scylla->prep_insert_message()) };
        ::cass_statement_bind_string(stmt.get(), 0, req.channel_id().c_str());
        ::cass_statement_bind_uuid(stmt.get(), 1, uuid);
        ::cass_statement_bind_string(stmt.get(), 2, sess->user_id.c_str());
        ::cass_statement_bind_string(stmt.get(), 3, req.content().c_str());

        // Copy everything we'll need in the callback BEFORE returning -- by
        // the time the driver IO thread fires us, `req` and `sess` are gone.
        auto ctx = std::make_unique<InsertCtx>();
        ctx->session_id = sess->session_id;
        ctx->loop = ::uWS::Loop::get();
        ctx->state = &state;
        ctx->fut.reset(::cass_session_execute(state.scylla->session(), stmt.get()));
        ctx->message_id = uuid_str;
        ctx->channel_id = req.channel_id();
        ctx->sender_id = sess->user_id;
        ctx->content = req.content();
        ctx->client_nonce = req.client_nonce();

        ::cass_future_set_callback(ctx->fut.get(), &on_insert_complete, ctx.get());
        ctx.release();

    }

    void on_fetch_history(GatewayState& state, WebSocket* ws, const ::signaling::FetchHistory& req)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

        // Cursor: empty => the literal maximum v1 TimeUUID. We can't use
        // cass_uuid_max_from_time(LLONG_MAX, ...) here because the driver
        // internally multiplies (ms * 10000) to convert to 100ns intervals,
        // and LLONG_MAX * 10000 overflows uint64 -- wrapping to a value
        // smaller than real TimeUUIDs and excluding all rows.
        ::CassUuid cursor { };
        if (!req.before_message_id().empty()) ::cass_uuid_from_string(req.before_message_id().c_str(), &cursor);
        else ::cass_uuid_from_string("ffffffff-ffff-1fff-bfff-ffffffffffff", &cursor);

        const std::int32_t limit { (req.limit() == 0 || req.limit() > 100) ? 100 : static_cast<std::int32_t>(req.limit()) };

        CassStatementPtr stmt { ::cass_prepared_bind(state.scylla->prep_fetch_history()) };
        ::cass_statement_bind_string(stmt.get(), 0, req.channel_id().c_str());
        ::cass_statement_bind_uuid(stmt.get(), 1, cursor);
        ::cass_statement_bind_int32(stmt.get(), 2, limit);

        auto ctx = std::make_unique<HistoryCtx>();
        ctx->session_id = sess->session_id;
        ctx->loop = ::uWS::Loop::get();
        ctx->state = &state;
        ctx->fut.reset(::cass_session_execute(state.scylla->session(), stmt.get()));
        ctx->request_id = req.request_id();
        ctx->channel_id = req.channel_id();
        ctx->before_message_id = req.before_message_id();
        ctx->limit = limit;

        ::cass_future_set_callback(ctx->fut.get(), &on_history_complete, ctx.get());
        ctx.release();

    }

    void on_join_voice(WebSocket* ws, const ::signaling::JoinVoice& req)
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

    void on_leave_voice(WebSocket* ws, const ::signaling::LeaveVoice& req)
    {

        ws->unsubscribe("voice:" + req.channel_id());
        // TODO: remove from voice_peers map, publish VoicePeerLeft.

    }

    void on_sdp_offer(GatewayState& state, WebSocket* ws, const ::signaling::Sdp& offer)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }
        if (state.sfu == nullptr) { send_error(ws, 503, "sfu unavailable"); return; }

        const std::string peer_id { make_peer_id(*sess) };
        ::signaling::Sdp answer { };
        std::string err { };
        if (!state.sfu->add_peer(offer.room_id(), peer_id, offer, &answer, &err))
        {

            std::cerr << "[sfu] AddPeer failed: " << err << '\n';
            send_error(ws, 502, "sfu add_peer failed");
            return;

        }

        ::signaling::Envelope envelope { };
        *envelope.mutable_sdp_answer() = answer;
        send_envelope(ws, envelope);

    }

    void on_ice_candidate(GatewayState& state, WebSocket* ws, const ::signaling::IceCandidate& candidate)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }
        if (state.sfu == nullptr) return;

        const std::string peer_id { make_peer_id(*sess) };
        state.sfu->add_remote_ice_candidate(candidate.room_id(), peer_id, candidate);

    }

    void on_sfu_peer_event(GatewayState& state, const ::sfu_control::PeerEvent& event)
    {

        // The peer_id encoding is "user_id:session_id" -- extract the session id
        // suffix so we can find the WS in live_sessions.
        const std::string& peer_id { event.peer_id() };
        const auto colon { peer_id.rfind(':') };
        if (colon == std::string::npos) { std::cerr << "[sfu] malformed peer_id: " << peer_id << '\n'; return; }
        std::string session_id { peer_id.substr(colon + 1) };
        std::string room_id { event.room_id() };

        auto* loop = ::uWS::Loop::get();
        if (loop == nullptr) return;

        SessionRegistry* sessions { state.sessions };

        if (event.has_local_ice_candidate())
        {

            ::signaling::IceCandidate ice { event.local_ice_candidate() };
            loop->defer([sessions, session_id = std::move(session_id), ice = std::move(ice)]() mutable
            {

                WebSocket* ws { sessions->lookup(session_id) };
                if (ws == nullptr) return;
                ::signaling::Envelope envelope { };
                *envelope.mutable_server_ice_candidate() = std::move(ice);
                send_envelope(ws, envelope);

            });
            return;

        }

        if (event.has_peer_ready() && event.peer_ready())
        {

            loop->defer([sessions, session_id = std::move(session_id), room_id = std::move(room_id)]()
            {

                WebSocket* ws { sessions->lookup(session_id) };
                if (ws == nullptr) return;
                ::signaling::Envelope envelope { };
                envelope.mutable_peer_ready()->set_room_id(room_id);
                send_envelope(ws, envelope);

            });

        }

    }

    void on_message(GatewayState& state, WebSocket* ws, std::string_view data, ::uWS::OpCode op)
    {

        if (op != ::uWS::OpCode::BINARY) { send_error(ws, 400, "binary frames only"); return; }

        ::signaling::Envelope envelope { };
        if (!envelope.ParseFromArray(data.data(), static_cast<int>(data.size())))
        {

            send_error(ws, 400, "malformed envelope");
            return;

        }

        switch (envelope.payload_case())
        {

            case ::signaling::Envelope::kHello: on_hello(state, ws, envelope.hello()); break;
            case ::signaling::Envelope::kSendMessage: on_send_message(state, ws, envelope.send_message()); break;
            case ::signaling::Envelope::kFetchHistory: on_fetch_history(state, ws, envelope.fetch_history()); break;
            case ::signaling::Envelope::kJoinVoice: on_join_voice(ws, envelope.join_voice()); break;
            case ::signaling::Envelope::kLeaveVoice: on_leave_voice(ws, envelope.leave_voice()); break;
            case ::signaling::Envelope::kSdpOffer: on_sdp_offer(state, ws, envelope.sdp_offer()); break;
            case ::signaling::Envelope::kIceCandidate: on_ice_candidate(state, ws, envelope.ice_candidate()); break;
            case ::signaling::Envelope::kHeartbeat: on_heartbeat(ws, envelope.heartbeat()); break;

            default:
                send_error(ws, 400, "unsupported payload");
                break;

        }

    }

}
