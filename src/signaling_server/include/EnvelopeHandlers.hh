#ifndef SIGNALING_SERVER_ENVELOPE_HANDLERS_HH
#define SIGNALING_SERVER_ENVELOPE_HANDLERS_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <cstdint>
#include <string_view>

// 3rd party headers
#include <uwebsockets/App.h>

// project headers
#include "Session.hh"
#include "proto/signaling.pb.h"
#include "proto/sfu_control.pb.h"


namespace OpenSocialNet::Signaling
{

    struct GatewayState;


    // Generic helper: serialize an envelope and ship it over the WS.
    void send_envelope(WebSocket* ws, const ::signaling::Envelope& envelope);

    // Build + send a top-level Error envelope.
    void send_error(WebSocket* ws, std::uint32_t code, std::string_view msg);

    // Per-frame handlers. Each one is called from the uWS event loop thread
    // by the dispatcher below. They cover the entire protocol surface area
    // of the gateway.
    void on_hello(GatewayState& state, WebSocket* ws, const ::signaling::Hello& hello);
    void on_heartbeat(WebSocket* ws, const ::signaling::Heartbeat& heartbeat);
    void on_send_message(GatewayState& state, WebSocket* ws, const ::signaling::SendMessage& req);
    void on_fetch_history(GatewayState& state, WebSocket* ws, const ::signaling::FetchHistory& req);
    void on_join_voice(WebSocket* ws, const ::signaling::JoinVoice& req);
    void on_leave_voice(WebSocket* ws, const ::signaling::LeaveVoice& req);
    void on_sdp_offer(GatewayState& state, WebSocket* ws, const ::signaling::Sdp& offer);
    void on_ice_candidate(GatewayState& state, WebSocket* ws, const ::signaling::IceCandidate& candidate);

    // Dispatched off the SFU event-stream reader thread. Bounces WS work
    // back onto the uWS loop via Loop::defer, the same pattern the Scylla
    // async continuations use.
    void on_sfu_peer_event(GatewayState& state, const ::sfu_control::PeerEvent& event);

    // Top-level dispatcher: parse the frame, switch on which oneof case is
    // set. This switch is literally the entire protocol surface area of the
    // gateway.
    void on_message(GatewayState& state, WebSocket* ws, std::string_view data, ::uWS::OpCode op);

}

#endif // SIGNALING_SERVER_ENVELOPE_HANDLERS_HH
