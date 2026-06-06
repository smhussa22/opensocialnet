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
    else
    {

        std::cerr << "[auth] WARNING: OPENSOCIALNET_AUTH_SECRET is unset; all Hello frames will be rejected\n";

    }

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

    // Kafka producer; consumer thread is started once we have the App.
    // KAFKA_BOOTSTRAP overrides the default ("localhost:19092" for laptop dev,
    // "kafka:19092" for the docker compose network).
    OpenSocialNet::Signaling::KafkaBus kafka { };
    kafka.init(env_or("KAFKA_BOOTSTRAP", "localhost:19092"));

    // Live session_id -> WS map.
    OpenSocialNet::Signaling::SessionRegistry sessions { };

    // Wire everything into the shared state holder before we hand it to handlers.
    state.scylla = &scylla;
    state.kafka = &kafka;
    state.sessions = &sessions;
    // Capture this thread's uWS Loop BEFORE any worker threads start so that
    // background paths (kafka consumer, the future native-voice relay control
    // path) can Loop::defer back onto the right loop instead of their own.
    state.ws_loop = ::uWS::Loop::get();

    ::uWS::App app { };
    state.app = &app;

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

            // TODO(native-voice): once the relay's gRPC control plane is wired,
            // if sess->current_voice_room_id is non-empty here we have to tell
            // the relay to drop this peer's UDP endpoint from its room table,
            // otherwise the room leaks an inactive subscriber forever.

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
