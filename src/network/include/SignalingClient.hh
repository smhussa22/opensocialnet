#ifndef SIGNALING_CLIENT_HH
#define SIGNALING_CLIENT_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 3rd party headers
#include "proto/signaling.pb.h"

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
    //
    // Threading model:
    //   - The main thread does the synchronous handshake calls
    //     (connect_and_hello + join_voice) up front.
    //   - After join_voice succeeds, start_event_reader() spawns a
    //     background thread that loops on recv_binary and parses
    //     VoicePeerJoined / VoicePeerLeft / HeartbeatAck events. The
    //     main thread can poll peers() at its leisure.
    //   - send_heartbeat() is the only thing main thread should send
    //     after the reader starts; it's guarded by m_send_mu so future
    //     additional senders don't clobber a frame mid-write. recv from
    //     the reader thread and send from main thread on the same fd
    //     are safe at the kernel layer.
    class SignalingClient
    {

    public:

        SignalingClient() = default;
        ~SignalingClient();

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

        // Spawn the background reader thread. Call once, after join_voice
        // succeeds. The thread terminates when recv fails (server closed
        // the connection) or when close() is invoked. Safe to call from
        // the main thread only.
        void start_event_reader();

        // Send a Heartbeat frame so the server's idleTimeout doesn't drop
        // us mid-call. Take the send mutex; recv from the reader thread
        // is unaffected. Returns false on any send error.
        bool send_heartbeat();

        // Send a chat SendMessage on a channel. Doubles as the transport
        // for ICE SDP exchange — the gateway persists it and fans the
        // resulting ChatMessageEvent out to every channel subscriber.
        // Serialised with the other senders via m_send_mu.
        bool send_chat(const std::string& channel_id, const std::string& content);

        // Register a sink for inbound ChatMessageEvent frames. Fires on
        // the reader thread; our own messages (echo + fanout) are
        // filtered out by sender_id. Call before start_event_reader().
        void set_chat_handler(std::function<void(const std::string& sender_id, const std::string& content)> handler) { m_on_chat = std::move(handler); }

        // Register a generic envelope sink — fires for EVERY inbound
        // envelope on the reader thread, including ones the
        // type-specific handlers above also cover. Used by the IPC
        // bridge to forward server events verbatim to the GUI process
        // (FriendRequestEvent, FriendListResponse, ChatMessageEvent,
        // Ready, etc.) without SignalingClient needing to know about
        // each event type individually.
        void set_envelope_handler(std::function<void(const ::signaling::Envelope&)> handler) { m_on_envelope = std::move(handler); }

        // Ship an arbitrary client->server envelope through the same
        // send mutex the typed sends use. The IPC bridge wraps this so
        // the GUI can issue SendFriendRequest / AcceptFriendRequest /
        // FetchFriends without the protocol surface area leaking out.
        bool send_envelope(const ::signaling::Envelope& env);

        // Snapshot of the current peer list (NOT including self). The
        // reader thread keeps this updated; main thread reads under
        // m_peers_mu. Returns a copy so the caller doesn't have to hold
        // the lock across iteration.
        std::vector<VoicePeerInfo> peers() const;

        const std::string& session_id() const noexcept { return m_session_id; }
        const std::string& user_id()    const noexcept { return m_user_id; }

        // The Ready envelope from the hello handshake. It arrives before
        // any envelope handler can be registered, so consumers that need
        // it (the GUI IPC bridge) replay it from here instead.
        const ::signaling::Envelope& ready_envelope() const noexcept { return m_ready_env; }

        // Stop the reader thread + close the WS. Idempotent.
        void close() noexcept;


    private:

        WebSocketClient            m_ws         { };  // owns the underlying WS connection
        std::string                m_user_id    { };  // copy of what we sent in Hello (for self-match in join_voice)
        std::string                m_session_id { };  // server-assigned session id from Ready
        ::signaling::Envelope      m_ready_env  { };  // full Ready envelope, replayed to the GUI IPC bridge

        std::thread                m_reader     { };  // joined in close() / dtor
        std::atomic<bool>          m_running    { false }; // reader exit flag

        mutable std::mutex         m_peers_mu   { };  // guards m_peers
        std::vector<VoicePeerInfo> m_peers      { };  // remote peers in our voice room

        std::mutex                 m_send_mu    { };  // serialises send_binary across send_heartbeat / send_chat

        std::function<void(const std::string&, const std::string&)> m_on_chat { }; // inbound chat sink (reader thread)
        std::function<void(const ::signaling::Envelope&)>           m_on_envelope { }; // generic envelope sink (reader thread)
        std::uint32_t              m_nonce      { 0 }; // client_nonce uniqueness counter, guarded by m_send_mu


        // Reader thread entry point. Loops on m_ws.recv_binary, parses
        // each envelope, updates m_peers on VoicePeerJoined/Left.
        void run_reader();

    };

}

#endif // SIGNALING_CLIENT_HH
