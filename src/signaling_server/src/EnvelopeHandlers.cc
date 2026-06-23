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
#include "JwksCache.hh"
#include "JwtVerifier.hh"
#include "KafkaBus.hh"
#include "ScyllaClient.hh"
#include "Session.hh"


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

        // Two auth paths share one Hello envelope: a Google OAuth ID token
        // (RS256 JWT, three dot-separated base64url segments) for real
        // users, or the legacy HMAC hex digest for dev/integration runs.
        // The shape of the token unambiguously picks the path.
        const bool jwt_path { looks_like_jwt(hello.auth_token()) };

        if (jwt_path)
        {

            if (state.google_client_id.empty() or state.jwks == nullptr)
            {

                send_error(ws, 401, "JWT auth not configured");
                ws->close();
                return;

            }

            const JwtVerifyResult verify { verify_google_id_token(*state.jwks, hello.auth_token(), state.google_client_id) };
            if (!verify.ok)
            {

                std::cerr << "[auth] JWT rejected: " << verify.error << '\n';
                send_error(ws, 401, "invalid ID token");
                ws->close();
                return;

            }

            // Use the Google `sub` as user_id — stable, unique, never
            // changes if the user changes their email. The display name is
            // best-effort: prefer profile name, fall back to email, fall
            // back to sub (so the row is never written with an empty
            // username).
            sess->user_id = verify.sub;
            std::string username { verify.name };
            if (username.empty()) username = verify.email;
            if (username.empty()) username = verify.sub;
            state.scylla->upsert_user(verify.sub, username);

            std::cerr << "[auth] JWT ok: sub=" << verify.sub << " email=" << verify.email << '\n';

        }
        else
        {

            // HMAC dev path — gated by env, never enabled in prod builds
            // once we ship.
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

                std::cerr << "[auth] HMAC denied user=" << hello.user_id() << '\n';
                send_error(ws, 401, "invalid auth token");
                ws->close();
                return;

            }

            sess->user_id = hello.user_id();

        }

        sess->session_id = make_session_id();
        sess->authenticated = true;

        // Register the session AFTER auth: async continuations look the socket
        // up by session_id, so pre-auth connections are deliberately invisible.
        state.sessions->add(sess->session_id, ws);

        std::cerr << "[hello] user=" << sess->user_id << " session=" << sess->session_id << '\n';

        // Auto-join the default `general` channel so a brand-new sign-in
        // has somewhere to chat without an explicit signup ceremony. Both
        // writes are idempotent — repeat sign-ins are cheap no-ops on the
        // partition keys.
        state.scylla->add_user_to_channel("general", sess->user_id);

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

    namespace
    {

        // Build + send a single VoicePeerJoined frame to a specific ws.
        // Helper because the same shape is sent four ways during a join:
        // self to joiner, each existing peer to joiner, joiner to each
        // existing peer, and (eventually) joiner to itself via topic
        // subscribe — keep them all consistent through one builder.
        void send_voice_peer_joined(WebSocket* ws, const std::string& channel_id, const std::string& user_id, const std::string& ip, std::uint16_t port, std::uint32_t ssrc)
        {

            ::signaling::Envelope out { };
            auto* event = out.mutable_voice_peer_joined();
            event->set_channel_id(channel_id);
            auto* peer = event->mutable_peer();
            peer->set_user_id(user_id);
            peer->set_ip(ip);
            peer->set_port(port);
            peer->set_ssrc(ssrc);
            send_envelope(ws, out);

        }

        void send_voice_peer_left(WebSocket* ws, const std::string& channel_id, const std::string& user_id)
        {

            ::signaling::Envelope out { };
            auto* event = out.mutable_voice_peer_left();
            event->set_channel_id(channel_id);
            event->set_user_id(user_id);
            send_envelope(ws, out);

        }

        // Pull `sess` out of `channel_id`'s membership list, return the
        // peers that remained. Caller is responsible for broadcasting
        // VoicePeerLeft to them — we don't do it here because the close
        // handler and on_leave_voice want the same effect but reach this
        // path from slightly different setups.
        std::vector<VoiceRoomPeer> remove_from_voice_room(GatewayState& state, const Session& sess, const std::string& channel_id)
        {

            std::vector<VoiceRoomPeer> remaining { };
            std::scoped_lock<std::mutex> lock { state.voice_rooms_mu };

            auto it = state.voice_rooms.find(channel_id);
            if (it == state.voice_rooms.end()) return remaining;

            auto& room = it->second;
            auto new_end = std::remove_if(room.begin(), room.end(), [&](const VoiceRoomPeer& p) { return p.session_id == sess.session_id; });
            room.erase(new_end, room.end());

            if (room.empty()) { state.voice_rooms.erase(it); return remaining; }

            remaining = room;
            return remaining;

        }

    }


    void leave_voice_room(GatewayState& state, WebSocket* ws, const std::string& channel_id)
    {

        auto* sess = ws->getUserData();
        if (sess == nullptr or channel_id.empty()) return;

        ws->unsubscribe("voice:" + channel_id);

        // Mirror of join's chat-topic subscribe. Harmless even if the
        // user was also subscribed via user_channels at Hello — today
        // that table is empty, and once it isn't, join/leave-voice should
        // become refcount-aware anyway.
        ws->unsubscribe(channel_id);
        const std::vector<VoiceRoomPeer> remaining { remove_from_voice_room(state, *sess, channel_id) };
        sess->current_voice_room_id.clear();

        // Notify each remaining peer that this user is gone. Direct sends
        // (vs publish) so we don't accidentally send VoicePeerLeft to a
        // socket that's already gone — sessions->lookup hands us back the
        // current ws* or nullptr if it died.
        for (const auto& peer : remaining)
        {

            WebSocket* peer_ws { state.sessions->lookup(peer.session_id) };
            if (peer_ws != nullptr) send_voice_peer_left(peer_ws, channel_id, sess->user_id);

        }

        std::cerr << "[leave_voice] user=" << sess->user_id << " channel=" << channel_id << " remaining=" << remaining.size() << '\n';

    }


    void on_join_voice(GatewayState& state, WebSocket* ws, const ::signaling::JoinVoice& req)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

        // Idempotency: if this session re-joins the same room (a client
        // restart that left a half-open connection on the server), kick
        // out the previous registration first so we don't end up with
        // two entries for one peer.
        if (!sess->current_voice_room_id.empty()) leave_voice_room(state, ws, sess->current_voice_room_id);

        // Voice events get their own topic so they don't intermingle with
        // chat consumers on the same channel_id. Subscribing here means
        // future VoicePeerJoined / VoicePeerLeft broadcasts published to
        // "voice:<channel>" will fan out to this socket automatically —
        // currently we direct-send instead, but the subscription is
        // future-proofing for the kafka-fanout path.
        const std::string channel { req.channel_id() };
        ws->subscribe("voice:" + channel);

        // Chat topic too: the Kafka fanout publishes ChatMessageEvents to
        // the bare channel name, and a voice participant needs those even
        // when user_channels (the Hello-time subscription source) doesn't
        // list them — in-room text chat and the ICE SDP exchange both
        // ride that path.
        ws->subscribe(channel);
        sess->current_voice_room_id = channel;

        const std::uint32_t assigned_ssrc { state.next_ssrc.fetch_add(1, std::memory_order_relaxed) };

        // Snapshot the existing peer list, then add self under the same
        // lock. After this point new joiners will see us; we already have
        // the existing snapshot to replay to the wire.
        std::vector<VoiceRoomPeer> existing { };
        {

            std::scoped_lock<std::mutex> lock { state.voice_rooms_mu };
            auto& room = state.voice_rooms[channel];
            existing = room;
            room.push_back({ sess->user_id, sess->session_id, assigned_ssrc });

        }

        // 1. Self first (the SignalingClient protocol contract — caller
        //    waits for its own entry to know the slot is ready).
        send_voice_peer_joined(ws, channel, sess->user_id, state.relay_host, state.relay_port, assigned_ssrc);

        // 2. Replay each existing peer to the joiner so the new client
        //    learns who is already in the room.
        for (const auto& peer : existing)
        {

            send_voice_peer_joined(ws, channel, peer.user_id, state.relay_host, state.relay_port, peer.ssrc);

        }

        // 3. Tell each existing peer about the new joiner.
        for (const auto& peer : existing)
        {

            WebSocket* peer_ws { state.sessions->lookup(peer.session_id) };
            if (peer_ws != nullptr) send_voice_peer_joined(peer_ws, channel, sess->user_id, state.relay_host, state.relay_port, assigned_ssrc);

        }

        std::cerr << "[join_voice] user=" << sess->user_id << " channel=" << channel << " ssrc=" << assigned_ssrc << " existing=" << existing.size() << " relay=" << state.relay_host << ":" << state.relay_port << '\n';

    }

    void on_leave_voice(GatewayState& state, WebSocket* ws, const ::signaling::LeaveVoice& req)
    {

        leave_voice_room(state, ws, req.channel_id());

    }


    // ============================================================
    // Friends / DMs
    // ============================================================

    namespace
    {

        // Deterministic DM channel id from a pair of user_ids. Sorting
        // first means both sides derive the same id without coordinating;
        // the `dm:` prefix keeps DMs visually distinct from group channels
        // when you scan the channels table by eye.
        std::string dm_channel_id_for(const std::string& a, const std::string& b)
        {

            const auto [lo, hi] { std::minmax(a, b) };
            return "dm:" + lo + "_" + hi;

        }


        // Deliver an envelope to every active session for `user_id`. Used
        // by friend request notifications — the recipient may be online on
        // one or more devices and we want all of them to update in real
        // time. Iterates under the registry's lock (snapshotted by
        // for_each) so multi-device fanout is one pass.
        void notify_user(GatewayState& state, const std::string& user_id, const ::signaling::Envelope& envelope)
        {

            std::string buf { };
            envelope.SerializeToString(&buf);

            state.sessions->for_each([&](const std::string&, WebSocket* peer_ws)
            {

                if (peer_ws == nullptr) return;
                auto* peer_sess { peer_ws->getUserData() };
                if (peer_sess->user_id == user_id) peer_ws->send(buf, ::uWS::OpCode::BINARY);

            });

        }

    }


    void on_send_friend_request(GatewayState& state, WebSocket* ws, const ::signaling::SendFriendRequest& req)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

        const std::string& to_user_id { req.to_user_id() };
        if (to_user_id.empty() or to_user_id == sess->user_id)
        {

            send_error(ws, 400, "invalid to_user_id");
            return;

        }

        if (!state.scylla->insert_friend_request(to_user_id, sess->user_id))
        {

            send_error(ws, 500, "could not record friend request");
            return;

        }
        std::cerr << "[friend] request " << sess->user_id << " -> " << to_user_id << '\n';

        // Live notification to the recipient (any session of theirs that
        // happens to be connected right now). If they're offline, the
        // request sits in friend_requests and surfaces on their next
        // FetchFriends.
        ::signaling::Envelope evt { };
        auto* event { evt.mutable_friend_request_event() };
        event->set_from_user_id(sess->user_id);
        event->set_from_username(state.scylla->username_for(sess->user_id));
        event->set_created_at_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        notify_user(state, to_user_id, evt);

    }


    void on_accept_friend_request(GatewayState& state, WebSocket* ws, const ::signaling::AcceptFriendRequest& req)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

        const std::string& from_user_id { req.from_user_id() };
        if (from_user_id.empty() or from_user_id == sess->user_id)
        {

            send_error(ws, 400, "invalid from_user_id");
            return;

        }

        // Materialize the friendship: two friendships rows + a DM channel
        // that both sides hold so chat fanout via Kafka routes correctly.
        const std::string dm_channel { dm_channel_id_for(sess->user_id, from_user_id) };
        if (!state.scylla->ensure_channel(dm_channel, dm_channel, "dm")
            or !state.scylla->add_user_to_channel(dm_channel, sess->user_id)
            or !state.scylla->add_user_to_channel(dm_channel, from_user_id)
            or !state.scylla->insert_friendship_pair(sess->user_id, from_user_id, dm_channel))
        {

            send_error(ws, 500, "could not materialize friendship");
            return;

        }
        state.scylla->delete_friend_request(sess->user_id, from_user_id);

        // Self subscribes to the new DM so messages route on this socket.
        ws->subscribe(dm_channel);

        std::cerr << "[friend] accept " << sess->user_id << " <-> " << from_user_id << " dm=" << dm_channel << '\n';

        // Notify the original requester (if online on any session) — and
        // subscribe each of their sockets to the DM so they receive
        // messages without waiting for a reconnect.
        ::signaling::Envelope evt { };
        auto* event { evt.mutable_friend_request_accepted_event() };
        event->set_user_id(sess->user_id);
        event->set_username(state.scylla->username_for(sess->user_id));
        event->set_dm_channel_id(dm_channel);

        std::string buf { };
        evt.SerializeToString(&buf);

        state.sessions->for_each([&](const std::string&, WebSocket* peer_ws)
        {

            if (peer_ws == nullptr) return;
            auto* peer_sess { peer_ws->getUserData() };
            if (peer_sess->user_id != from_user_id) return;
            peer_ws->subscribe(dm_channel);
            peer_ws->send(buf, ::uWS::OpCode::BINARY);

        });

    }


    void on_reject_friend_request(GatewayState& state, WebSocket* ws, const ::signaling::RejectFriendRequest& req)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

        const std::string& from_user_id { req.from_user_id() };
        if (from_user_id.empty()) { send_error(ws, 400, "invalid from_user_id"); return; }

        state.scylla->delete_friend_request(sess->user_id, from_user_id);
        std::cerr << "[friend] reject " << sess->user_id << " <- " << from_user_id << '\n';

    }


    void on_fetch_friends(GatewayState& state, WebSocket* ws, const ::signaling::FetchFriends& /*req*/)
    {

        auto* sess = ws->getUserData();
        if (!sess->authenticated) { send_error(ws, 401, "not authenticated"); return; }

        ::signaling::Envelope envelope { };
        auto* resp { envelope.mutable_friend_list_response() };

        for (const auto& f : state.scylla->list_friendships(sess->user_id))
        {

            auto* fs { resp->add_friends() };
            fs->set_user_id(f.friend_user_id);
            fs->set_username(state.scylla->username_for(f.friend_user_id));
            fs->set_dm_channel_id(f.dm_channel_id);
            fs->set_since_ms(f.since_ms);

        }
        for (const auto& p : state.scylla->list_friend_requests(sess->user_id))
        {

            auto* in { resp->add_incoming() };
            in->set_from_user_id(p.from_user_id);
            in->set_from_username(state.scylla->username_for(p.from_user_id));
            in->set_created_at_ms(p.created_at_ms);

        }
        send_envelope(ws, envelope);

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

            case ::signaling::Envelope::kHello: 
                on_hello(state, ws, envelope.hello()); 
                break;

            case ::signaling::Envelope::kSendMessage: 
                on_send_message(state, ws, envelope.send_message()); 
                break;

            case ::signaling::Envelope::kFetchHistory: 
                on_fetch_history(state, ws, envelope.fetch_history()); 
                break;

            case ::signaling::Envelope::kJoinVoice:
                on_join_voice(state, ws, envelope.join_voice());
                break;

            case ::signaling::Envelope::kLeaveVoice:
                on_leave_voice(state, ws, envelope.leave_voice());
                break;

            case ::signaling::Envelope::kHeartbeat:
                on_heartbeat(ws, envelope.heartbeat());
                break;

            case ::signaling::Envelope::kSendFriendRequest:
                on_send_friend_request(state, ws, envelope.send_friend_request());
                break;

            case ::signaling::Envelope::kAcceptFriendRequest:
                on_accept_friend_request(state, ws, envelope.accept_friend_request());
                break;

            case ::signaling::Envelope::kRejectFriendRequest:
                on_reject_friend_request(state, ws, envelope.reject_friend_request());
                break;

            case ::signaling::Envelope::kFetchFriends:
                on_fetch_friends(state, ws, envelope.fetch_friends());
                break;

            default:
                send_error(ws, 400, "unsupported payload");
                break;

        }

    }

}
