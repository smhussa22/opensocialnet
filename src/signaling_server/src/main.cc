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
#include "SfuClient.hh"
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

    // Scylla connection + prepared statements.
    OpenSocialNet::Signaling::ScyllaClient scylla { };
    scylla.init("127.0.0.1");

    // Kafka producer; consumer thread is started once we have the App.
    OpenSocialNet::Signaling::KafkaBus kafka { };
    kafka.init("localhost:19092");

    // SFU gRPC control-plane client. One stub for the process lifetime; gRPC
    // handles concurrency. The event stream runs on its own background thread
    // and bounces WS writes back onto the uWS loop via Loop::defer.
    auto sfu = std::make_unique<OpenSocialNet::Signaling::SfuClient>("127.0.0.1:50051");

    // Live session_id -> WS map.
    OpenSocialNet::Signaling::SessionRegistry sessions { };

    // Wire everything into the shared state holder before we hand it to handlers.
    state.scylla = &scylla;
    state.kafka = &kafka;
    state.sfu = sfu.get();
    state.sessions = &sessions;

    // The SFU event reader runs on its own thread inside SfuClient. The handler
    // captures `state` by reference; main()'s stack outlives that thread because
    // we call shutdown() before returning.
    sfu->start_event_stream([&state](const ::sfu_control::PeerEvent& event)
    {

        OpenSocialNet::Signaling::on_sfu_peer_event(state, event);

    });

    ::uWS::App app { };
    state.app = &app;

    // Start the Kafka consumer once we have an App handle to publish into.
    kafka.start_consumer(&app, "localhost:19092");

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
    sfu->shutdown();
    kafka.shutdown();
    return 0;

}
