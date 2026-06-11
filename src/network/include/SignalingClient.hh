#ifndef SIGNALING_CLIENT_HH
#define SIGNALING_CLIENT_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <string>
#include <vector>

// 3rd party headers

// project headers
#include "WebSocketClient.hh"


namespace OpenSocialNet::Network
{

    // One row of a VoicePeerJoined response. For a relay topology, every
    // peer.ip / peer.port in the room is the same — it's the relay's
    // public UDP endpoint — but the per-peer identity (user_id + ssrc)
    // varies.
    struct VoicePeerInfo
    {

        std::string   user_id { }; // who this peer is
        std::string   ip      { }; // UDP host we send to (relay's public IP for relay topology)
        std::uint16_t port    { 0 };
        std::uint32_t ssrc    { 0 }; // identifier this peer stamps into Packet.header.ssrc

    };


    // Thin orchestration on top of WebSocketClient: walks the
    // Hello → Ready → JoinVoice → VoicePeerJoined(self) state machine on
    // the signaling gateway and surfaces the relay endpoint to the caller.
    class SignalingClient
    {

    public:

        SignalingClient() = default;
        ~SignalingClient() = default;

        SignalingClient(const SignalingClient&)            = delete;
        SignalingClient& operator=(const SignalingClient&) = delete;
        SignalingClient(SignalingClient&&)                 = delete;
        SignalingClient& operator=(SignalingClient&&)      = delete;

        // Connect WS, send Hello(user_id, auth_token), wait for Ready.
        // Stores the assigned session_id. Returns false on any error.
        bool connect_and_hello(const std::string& host, std::uint16_t port, const std::string& path, const std::string& user_id, const std::string& auth_token);

        // Send JoinVoice(channel_id), wait for VoicePeerJoined whose
        // peer.user_id matches our own user_id. That entry tells us where
        // the relay is and what ssrc the server assigned us. Other peers
        // that arrive in the same response are appended to `others`.
        // Returns false on any error / mismatched envelope.
        bool join_voice(const std::string& channel_id, VoicePeerInfo& self_out, std::vector<VoicePeerInfo>& others_out);

        const std::string& session_id() const noexcept { return m_session_id; }
        const std::string& user_id()    const noexcept { return m_user_id; }

        void close() noexcept { m_ws.close(); }


    private:

        WebSocketClient m_ws         { };  // owns the underlying WS connection
        std::string     m_user_id    { };  // copy of what we sent in Hello (for self-match in join_voice)
        std::string     m_session_id { };  // server-assigned session id from Ready

    };

}

#endif // SIGNALING_CLIENT_HH
