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

    // Browser is telling us a specific m-line in its next SDP offer carries
    // screen-capture video (or that it has stopped sharing). Must arrive BEFORE
    // the renegotiated offer so the SFU labels the mid correctly. The handler
    // (a) calls SfuClient::mark_screen_share so the SFU's SfuPeer dispatches
    // the new track's RTP to its screen handler, and (b) fans the update out
    // to every other peer in the room as Envelope.peer_screen_share so their
    // UIs can react.
    void on_screen_share_update(GatewayState& state, WebSocket* ws, const ::signaling::ScreenShareUpdate& update);

    // Browser's SDP answer to an SFU-initiated renegotiation offer (e.g. the
    // server_sdp_offer we shipped down when another peer started screen
    // sharing). Forwarded straight into SfuClient::accept_renegotiation_answer
    // so the SFU can setRemoteDescription and finalize the renegotiation.
    void on_client_sdp_answer(GatewayState& state, WebSocket* ws, const ::signaling::Sdp& answer);

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
