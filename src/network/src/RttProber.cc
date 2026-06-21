// related headers
#include "RttProber.hh"

// c sys headers

// cpp stdlib headers
#include <cstdio>
#include <cstring>

// 3rd party headers

// project headers

namespace
{

    // RFC 6298 §2.3 — classic TCP SRTT smoothing factor (1/8). Same value
    // every UDP/RTP stack reaches for; aggressive enough to track route
    // changes within a few samples, conservative enough that one bad
    // sample doesn't yank the displayed RTT around.
    constexpr double srtt_alpha { 0.125 };

    // Wire payload of a Ping/Pong: 8 bytes = one int64 nanosecond count
    // from the sender's steady_clock at send time. The receiver echoes
    // these 8 bytes verbatim so the sender can close the loop with
    // now - send_ns without any matching table.
    constexpr std::uint16_t rtt_payload_size { 8 };

}

namespace OpenSocialNet::Network
{

    RttProber::RttProber(std::chrono::milliseconds ping_interval) noexcept
        : m_ping_interval { ping_interval }
    {

    }


    bool RttProber::should_send_ping(std::chrono::steady_clock::time_point now) noexcept
    {

        // First ever call: m_last_ping_at default-constructs to the epoch,
        // so the difference is huge and we fire immediately.
        return now - m_last_ping_at >= m_ping_interval;

    }


    Packet RttProber::build_ping(std::uint64_t room_id, std::uint32_t self_peer_id, std::chrono::steady_clock::time_point now) noexcept
    {

        m_last_ping_at = now;

        Packet packet { };
        packet.header.version      = packet_protocol_version;
        packet.header.flags        = 0;
        packet.header.payload_type = PayloadType::RttPing;
        packet.header.room_id      = room_id;
        packet.header.peer_id      = self_peer_id;
        packet.header.ssrc         = 0;
        packet.header.timestamp    = 0;
        packet.header.sequence     = 0;
        packet.header.payload_size = rtt_payload_size;

        const std::int64_t send_ns { now.time_since_epoch().count() };
        std::memcpy(packet.payload, &send_ns, sizeof(send_ns));

        return packet;

    }


    Packet RttProber::build_pong(const Packet& ping_in, std::uint32_t self_peer_id) const noexcept
    {

        Packet pong { };
        pong.header.version      = packet_protocol_version;
        pong.header.flags        = 0;
        pong.header.payload_type = PayloadType::RttPong;
        pong.header.room_id      = ping_in.header.room_id;   // same room — relay fans back to the original sender
        pong.header.peer_id      = self_peer_id;             // our identity — so the original sender knows whose RTT this is
        pong.header.ssrc         = 0;
        pong.header.timestamp    = 0;
        pong.header.sequence     = 0;
        pong.header.payload_size = rtt_payload_size;

        // Critical: payload mirrored byte-for-byte. The original sender
        // reads it back as its own send_ns, no matching table needed.
        std::memcpy(pong.payload, ping_in.payload, rtt_payload_size);

        return pong;

    }


    void RttProber::on_pong(std::uint32_t remote_peer_id, const Packet& pong_in, std::chrono::steady_clock::time_point now)
    {

        // Defensive: payload size must be exactly 8. A malformed pong is a
        // wire-format bug, not user input — skip silently rather than
        // poisoning the EWMA with garbage.
        if (pong_in.header.payload_size != rtt_payload_size) return;

        std::int64_t send_ns { 0 };
        std::memcpy(&send_ns, pong_in.payload, sizeof(send_ns));

        const std::int64_t now_ns    { now.time_since_epoch().count() };
        const std::int64_t rtt_ns    { now_ns - send_ns };
        if (rtt_ns < 0) return; // clock went backwards or stale pong from before a reset
        const double       rtt_ms_d { static_cast<double>(rtt_ns) / 1'000'000.0 };

        std::scoped_lock<std::mutex> lock { m_mu };
        PeerRtt& bucket { m_peers[remote_peer_id] };
        bucket.last_seen = now;
        if (!bucket.has_sample)
        {

            bucket.ewma_ms    = rtt_ms_d;
            bucket.has_sample = true;

        }
        else
        {

            bucket.ewma_ms = (1.0 - srtt_alpha) * bucket.ewma_ms + srtt_alpha * rtt_ms_d;

        }

    }


    std::optional<double> RttProber::rtt_ms(std::uint32_t remote_peer_id) const
    {

        std::scoped_lock<std::mutex> lock { m_mu };
        const auto it { m_peers.find(remote_peer_id) };
        if (it == m_peers.end() or !it->second.has_sample) return std::nullopt;
        return it->second.ewma_ms;

    }


    std::string RttProber::format_for_stats() const
    {

        std::scoped_lock<std::mutex> lock { m_mu };
        if (m_peers.empty()) return { };

        std::string out { };
        out.reserve(m_peers.size() * 24);
        bool first { true };
        for (const auto& [pid, bucket] : m_peers)
        {

            if (!bucket.has_sample) continue;
            char chunk[40] { };
            std::snprintf(chunk, sizeof(chunk), "%s%08x=%.1fms", first ? "" : " ", pid, bucket.ewma_ms);
            out.append(chunk);
            first = false;

        }
        return out;

    }

}
