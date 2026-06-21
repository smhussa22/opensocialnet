#ifndef RTT_PROBER_HH
#define RTT_PROBER_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

// 3rd party headers

// project headers
#include "Packet.hh"

namespace OpenSocialNet::Network
{

    // Bidirectional RTT measurement piggy-backing on the same UDP path
    // the media rides. Each side periodically emits an RttPing carrying
    // its current steady_clock ns in the 8-byte payload. The receiver
    // flips the type to RttPong and sends it straight back, payload
    // untouched. When the Pong returns, RTT = now() - payload_ns; that
    // sample feeds an RFC 6298 EWMA per remote peer.
    //
    // The prober never knows about transports: callers ask it for a
    // Packet via build_ping() / build_pong() and route the bytes through
    // whatever sender they have at hand (UdpSender for the relay path,
    // IceTransport for the P2P path). That keeps the prober reusable
    // across both lanes with zero coupling.
    class RttProber
    {

    public:

        explicit RttProber(std::chrono::milliseconds ping_interval = std::chrono::milliseconds { 1000 }) noexcept;

        RttProber(const RttProber&)            = delete;
        RttProber& operator=(const RttProber&) = delete;
        RttProber(RttProber&&)                 = delete;
        RttProber& operator=(RttProber&&)      = delete;


        // True when the configured interval since the last sent ping has
        // elapsed. Stateful — calling it has no effect, only build_ping()
        // resets the timer (so a caller that decides not to actually send
        // doesn't lose its slot).
        bool should_send_ping(std::chrono::steady_clock::time_point now) noexcept;


        // Stamps a fresh Ping packet with this peer's identity and the
        // current steady_clock ns in the 8-byte payload. Resets the
        // internal interval timer. The caller is responsible for shipping
        // the returned packet via whatever transport they prefer.
        Packet build_ping(std::uint64_t room_id, std::uint32_t self_peer_id, std::chrono::steady_clock::time_point now) noexcept;


        // Builds a Pong from an inbound Ping. The Pong's payload mirrors
        // the Ping's exactly (that's what closes the loop on the sender's
        // side), but the peer_id is rewritten to self_peer_id so the
        // original sender sees which peer responded.
        Packet build_pong(const Packet& ping_in, std::uint32_t self_peer_id) const noexcept;


        // Consume a returning Pong. Reads the 8-byte payload as the
        // original send_ns, computes one RTT sample, folds it into the
        // EWMA bucket keyed by remote_peer_id.
        void on_pong(std::uint32_t remote_peer_id, const Packet& pong_in, std::chrono::steady_clock::time_point now);


        // Latest smoothed RTT in milliseconds for one peer, or none if
        // no Pong has come back yet.
        std::optional<double> rtt_ms(std::uint32_t remote_peer_id) const;


        // "peer_id=4.2ms peer_id=27.8ms" style fragment for the
        // [net-stats] log line. Empty string when no samples yet.
        std::string format_for_stats() const;


    private:

        struct PeerRtt
        {

            double                                ewma_ms    { 0.0 };   // RFC 6298 style smoothed estimate, ms
            bool                                  has_sample { false }; // false until first Pong returns
            std::chrono::steady_clock::time_point last_seen  { };       // for future stale-entry pruning

        };

        std::chrono::milliseconds             m_ping_interval { };  // throttle for should_send_ping()
        std::chrono::steady_clock::time_point m_last_ping_at  { };  // updated only inside build_ping()

        mutable std::mutex                                m_mu    { }; // guards m_peers across recv thread + stats reader
        std::unordered_map<std::uint32_t, PeerRtt>        m_peers { }; // remote peer_id -> smoothed RTT bucket

    };

}

#endif // RTT_PROBER_HH
