#ifndef SIGNALING_SERVER_GATEWAY_STATE_HH
#define SIGNALING_SERVER_GATEWAY_STATE_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <string>

// 3rd party headers
#include <uwebsockets/App.h>

// project headers


namespace OpenSocialNet::Signaling
{

    class ScyllaClient;
    class KafkaBus;
    class SessionRegistry;


    // Central holder of process-wide runtime references that every WS
    // handler needs. Lives on main()'s stack and is passed by reference
    // (or pointer, via the App user-data slot) into the envelope handlers.
    // Each pointer is non-owning; ownership of each subsystem stays in
    // main().
    struct GatewayState
    {

        ::uWS::App* app { nullptr }; // borrowed: needed so async paths can publish
        ::uWS::Loop* ws_loop { nullptr }; // borrowed: the uWS loop running the gateway; captured on the WS thread so worker threads (kafka, future native-voice relay control) can Loop::defer back onto it
        ScyllaClient* scylla { nullptr }; // borrowed: Scylla connection + prepared statements
        KafkaBus* kafka { nullptr }; // borrowed: producer + consumer thread
        SessionRegistry* sessions { nullptr }; // borrowed: live session_id -> WS map
        std::string auth_secret { }; // HMAC key for Hello auth; empty means "not configured"

    };

}

#endif // SIGNALING_SERVER_GATEWAY_STATE_HH
