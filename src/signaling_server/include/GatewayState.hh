#ifndef SIGNALING_SERVER_GATEWAY_STATE_HH
#define SIGNALING_SERVER_GATEWAY_STATE_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <atomic>
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

        // Relay endpoint shipped back to clients on JoinVoice. Same value
        // for every peer in every room — the relay does fan-out by
        // (room_id, peer_id) from the Packet header, so all clients dial
        // the same UDP socket and the relay sorts them out.
        std::string   relay_host { };       // OSN_RELAY_HOST, e.g. "3.144.229.204"
        std::uint16_t relay_port { 50100 }; // OSN_RELAY_PORT, defaults to the relay's bind port

        // Monotonic SSRC issuer. Each JoinVoice grabs a fresh value so
        // two peers (or two streams from the same peer) never collide.
        // Atomic so multi-thread futures don't race; in practice today
        // only the WS loop touches it.
        std::atomic<std::uint32_t> next_ssrc { 1 };

    };

}

#endif // SIGNALING_SERVER_GATEWAY_STATE_HH
