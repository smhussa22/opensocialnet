#ifndef ICE_TRANSPORT_HH
#define ICE_TRANSPORT_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <atomic>
#include <functional>
#include <span>
#include <string>
#include <thread>

// 3rd party headers
#include <agent.h>

// project headers
#include "IceDeleters.hh"

namespace OpenSocialNet::Network
{

    // Where the STUN/TURN infrastructure lives. An empty host disables
    // that candidate class — host candidates alone still connect peers
    // on the same machine or LAN.
    struct IceConfig
    {

        std::string   stun_host { };      // STUN server ("" = no server-reflexive candidates)
        std::uint16_t stun_port { 3478 }; // STUN UDP port
        std::string   turn_host { };      // TURN server ("" = no relayed candidates)
        std::uint16_t turn_port { 3478 }; // TURN UDP port
        std::string   turn_user { };      // TURN long-term credential username
        std::string   turn_pass { };      // TURN long-term credential password

    };


    // ICE (RFC 5245) connectivity via libnice: one peer-to-peer datagram
    // lane (single stream, single component) that the project's own
    // Packet wire format rides on unchanged — this class is strictly the
    // connectivity layer, it never looks inside a datagram.
    //
    // Threading model: libnice needs a GLib main context pumped somewhere,
    // so init() spawns a dedicated thread iterating a private context.
    // Every libnice callback (gathering done, state changes, inbound
    // data) fires on that thread; inbound datagrams are handed to the
    // caller's RecvCallback from there. send() is safe from any thread
    // (libnice agents lock internally).
    class IceTransport
    {

    public:
        using RecvCallback = std::function<void(std::span<const std::uint8_t>)>;

        IceTransport() noexcept = default;
        ~IceTransport() { shutdown(); }

        IceTransport(const IceTransport&) = delete;
        IceTransport& operator=(const IceTransport&) = delete;
        IceTransport(IceTransport&&) = delete;
        IceTransport& operator=(IceTransport&&) = delete;

        // Builds the agent + its loop thread and starts candidate
        // gathering. on_recv fires on the loop thread for every inbound
        // datagram once a pair connects. Returns false on setup failure.
        bool init(const IceConfig& config, RecvCallback on_recv) noexcept;

        // Blocks until candidate gathering finishes or timeout_ms passes.
        // The local description is only complete after this returns true.
        bool wait_gathered(int timeout_ms) noexcept;

        // SDP blob (credentials + candidates) to hand to the remote peer.
        [[nodiscard]] std::string local_description() const noexcept;

        // Ingests the remote peer's SDP and starts connectivity checks.
        // controlling settles the ICE role deterministically — pass
        // (our user id < their user id) on both sides.
        bool start_checks(const std::string& remote_sdp, bool controlling) noexcept;

        // Blocks until the component reaches CONNECTED/READY, fails fast
        // on FAILED, gives up after timeout_ms.
        bool wait_ready(int timeout_ms) noexcept;

        // Ships one datagram to the peer. False unless connected.
        bool send(std::span<const std::uint8_t> data) noexcept;

        // returns whether a candidate pair is currently usable.
        [[nodiscard]] bool connected() const noexcept;

        // Stops the loop thread, releases the agent. Idempotent.
        void shutdown() noexcept;

    private:
        GMainContextPtr context { };               // private context so we don't fight other GLib users
        NiceAgentPtr agent { };                    // the libnice agent
        std::thread loop_thread { };               // pumps the context; all libnice callbacks fire here
        std::atomic<bool> loop_running { false };  // loop thread exit flag
        ::guint stream_id { 0 };                   // the single negotiated stream
        std::atomic<bool> gathered { false };      // candidate-gathering-done fired
        std::atomic<unsigned> state { 0 };         // latest NiceComponentState
        RecvCallback on_recv { };                  // inbound datagram sink (loop thread)

        // libnice signal trampolines (user_data = this, loop thread).
        static void on_gathering_done(::NiceAgent* agent, ::guint stream, ::gpointer self) noexcept;
        static void on_state_changed(::NiceAgent* agent, ::guint stream, ::guint component, ::guint new_state, ::gpointer self) noexcept;
        static void on_pair_selected(::NiceAgent* agent, ::guint stream, ::guint component, ::NiceCandidate* local, ::NiceCandidate* remote, ::gpointer self) noexcept;
        static void on_data(::NiceAgent* agent, ::guint stream, ::guint component, ::guint size, ::gchar* buffer, ::gpointer self) noexcept;

    };

}

#endif // ICE_TRANSPORT_HH
