// related headers

// c sys headers
#include <cstdlib>

// cpp stdlib headers
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

// 3rd party headers
#include <uwebsockets/App.h>

// project headers
#include "EnvelopeHandlers.hh"
#include "GatewayState.hh"
#include "JwksCache.hh"
#include "KafkaBus.hh"
#include "ScyllaClient.hh"
#include "Session.hh"
#include "proto/signaling.pb.h"

int main()
{

    // to verify protobuf library version compatibility; dont rmeove
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    // Read once: every Hello handler reads state.auth_secret directly,
    // and we never reload at runtime.
    OpenSocialNet::Signaling::GatewayState state { };
    if (const char* secret = std::getenv("OPENSOCIALNET_AUTH_SECRET"); secret && *secret)
    {

        state.auth_secret = secret;

    }

    // Google OAuth client_id for Hello frames carrying an ID token. When
    // unset, the gateway falls back to HMAC-only — useful for local dev
    // / CI where there's no Google project. In a published build this
    // should always be set; the HMAC path should compile out.
    if (const char* client_id = std::getenv("OSN_GOOGLE_CLIENT_ID"); client_id && *client_id)
    {

        state.google_client_id = client_id;
        std::cerr << "[auth] Google client_id configured (JWT path enabled)\n";

    }

    if (state.auth_secret.empty() && state.google_client_id.empty())
    {

        std::cerr << "[auth] WARNING: neither OPENSOCIALNET_AUTH_SECRET nor OSN_GOOGLE_CLIENT_ID is set; all Hello frames will be rejected\n";

    }

    // Relay endpoint shipped back to every JoinVoice. Required for voice
    // to actually work; if unset, the server still runs but JoinVoice
    // replies with an empty IP and clients will fail to dial out.
    if (const char* relay = std::getenv("OSN_RELAY_HOST"); relay && *relay)
    {

        state.relay_host = relay;

    }
    else
    {

        std::cerr << "[voice] WARNING: OSN_RELAY_HOST is unset; JoinVoice replies will be unusable\n";

    }
    if (const char* relay_port = std::getenv("OSN_RELAY_PORT"); relay_port && *relay_port)
    {

        try { state.relay_port = static_cast<std::uint16_t>(std::stoi(relay_port)); }
        catch (...) { std::cerr << "[voice] OSN_RELAY_PORT not a number, keeping default " << state.relay_port << '\n'; }

    }
    std::cerr << "[voice] relay endpoint = " << state.relay_host << ":" << state.relay_port << '\n';

    // small env-or-default helper: env var wins if set + non-empty, else use the fallback.
    const auto env_or = [](const char* name, const char* fallback) -> std::string
    {

        const char* value { std::getenv(name) };
        return (value != nullptr && *value != '\0') ? std::string { value } : std::string { fallback };

    };

    // Scylla connection + prepared statements. SCYLLA_HOST overrides the default
    // (localhost for laptop dev, "scylla" container-DNS name in compose.prod.yml).
    OpenSocialNet::Signaling::ScyllaClient scylla { };
    scylla.init(env_or("SCYLLA_HOST", "127.0.0.1"));

    // Make sure the default `general` channel exists. Idempotent INSERT;
    // on_hello auto-adds every fresh sign-in to it so a brand-new user
    // has somewhere to chat without an explicit signup ceremony.
    scylla.ensure_channel("general", "general", "text");

    // Kafka producer; consumer thread is started once we have the App.
    // KAFKA_BOOTSTRAP overrides the default ("localhost:19092" for laptop dev,
    // "kafka:19092" for the docker compose network).
    OpenSocialNet::Signaling::KafkaBus kafka { };
    kafka.init(env_or("KAFKA_BOOTSTRAP", "localhost:19092"));

    // Live session_id -> WS map.
    OpenSocialNet::Signaling::SessionRegistry sessions { };

    // Google JWKS cache. Lazily fetches on the first JWT Hello; safe to
    // construct unconditionally — empty client_id just means the cache
    // is never consulted.
    OpenSocialNet::Signaling::JwksCache jwks { };

    // Wire everything into the shared state holder before we hand it to handlers.
    state.scylla = &scylla;
    state.kafka = &kafka;
    state.sessions = &sessions;
    state.jwks = &jwks;
    // Capture this thread's uWS Loop BEFORE any worker threads start so that
    // background paths (kafka consumer, the future native-voice relay control
    // path) can Loop::defer back onto the right loop instead of their own.
    state.ws_loop = ::uWS::Loop::get();

    ::uWS::App app { };
    state.app = &app;

    // Serve the OAuth client id/secret over plain HTTP so native clients
    // on fresh machines can sign in without a local .secrets file. The
    // desktop-app client_secret is non-confidential per Google's docs
    // (it ships inside any distributed binary anyway). The body is
    // KEY=VALUE lines — the same format the client's env-file parser
    // already understands. 404s when the gateway itself has no config.
    const std::string oauth_client_id     { env_or("OSN_GOOGLE_CLIENT_ID",     "") };
    const std::string oauth_client_secret { env_or("OSN_GOOGLE_CLIENT_SECRET", "") };
    app.get("/oauth_config", [oauth_client_id, oauth_client_secret](auto* res, auto* /*req*/)
    {

        if (oauth_client_id.empty())
        {

            res->writeStatus("404 Not Found")->end("no oauth config on this gateway\n");
            return;

        }
        res->writeHeader("Content-Type", "text/plain")->end("OSN_GOOGLE_CLIENT_ID=" + oauth_client_id + "\nOSN_GOOGLE_CLIENT_SECRET=" + oauth_client_secret + "\n");

    });

    // Start the Kafka consumer once we have an App handle to publish into.
    kafka.start_consumer(&app, env_or("KAFKA_BOOTSTRAP", "localhost:19092"));

    app.ws<OpenSocialNet::Signaling::Session>("/gateway",
    {
        .compression = ::uWS::DISABLED,
        .maxPayloadLength = 16 * 1024,
        .idleTimeout = 120,

        .open = [](OpenSocialNet::Signaling::WebSocket* /*ws*/)
        {

            // Session isn't registered yet -- we don't have a session_id until
            // Hello succeeds. Pre-auth connections can't be the target of async
            // continuations anyway, so being invisible to lookups is fine.
            std::cout << "ws open\n";

        },

        .message = [&state](OpenSocialNet::Signaling::WebSocket* ws, std::string_view data, ::uWS::OpCode op)
        {

            OpenSocialNet::Signaling::on_message(state, ws, data, op);

        },

        .close = [&state](OpenSocialNet::Signaling::WebSocket* ws, int code, std::string_view)
        {

            auto* sess = ws->getUserData();

            // If the connection dropped while still in a voice room, run
            // the same teardown LeaveVoice would: remove from the room's
            // membership list and broadcast VoicePeerLeft to remaining
            // peers. Has to happen BEFORE sessions->remove so the
            // remaining-peers lookup loop inside leave_voice_room can
            // still see other rooms' sessions (and it's fine that this
            // session's own lookup would race with the upcoming remove —
            // we never look ourselves up here).
            if (!sess->current_voice_room_id.empty())
            {

                OpenSocialNet::Signaling::leave_voice_room(state, ws, sess->current_voice_room_id);

            }

            // TODO(native-voice): once the relay's gRPC control plane is wired,
            // also tell the relay to drop this peer's UDP endpoint from its
            // room table so it doesn't fan out to a now-dead subscriber.

            if (!sess->session_id.empty()) state.sessions->remove(sess->session_id);
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
    kafka.shutdown();
    return 0;

}
