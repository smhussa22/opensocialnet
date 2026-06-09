// related headers
#include "PacketJitterBuffer.hh"

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

// 3rd party headers

// project headers
#include "Packet.hh"

namespace OpenSocialNet::Network
{

    bool PacketJitterBuffer::push(Packet& packet) noexcept
    {

        m_stats.observe(packet);

        std::scoped_lock<std::mutex> lock { mutex };
        if (playing && packet_sequence_less_than(packet.header.sequence, next_sequence)) return false;
        if (buffer.size() >= 50) return false;

        buffer.emplace(packet.header.sequence, packet);
        return true;

    }

    bool PacketJitterBuffer::pop(Packet& out) noexcept
    {

        std::scoped_lock<std::mutex> lock { mutex };

        if (!playing)
        {

            if (buffer.size() < playout_threshold) return false;
            playing = true;
            next_sequence = buffer.begin()->first;

        }

        auto it = buffer.find(next_sequence);
        if (it == buffer.end())
        {

            // Two skip conditions, each catches a different loss pattern:
            //
            // 1. Pile-up skip: buffer is well above the playout threshold,
            //    so newer packets are stacking up while we're stalled
            //    waiting for next_sequence. That's the single-frame-loss
            //    signature — head is lost, everything behind has arrived.
            //    Give up on the head, advance to the smallest waiting seq.
            //
            // 2. Burst skip: buffer's smallest waiting seq is 5+ ahead of
            //    next_sequence, meaning we lost a run of consecutive
            //    packets. Same fix — jump to wherever the buffer actually
            //    starts.
            //
            // The pile-up skip also paper-overs the previous "buffer hits
            // its 50-packet cap and silently drops new pushes" failure
            // mode that wedged the live binary on any single-frame loss.
            const bool pile_up_skip { buffer.size() >= playout_threshold + 3 };
            const bool burst_skip   { !buffer.empty() and packet_sequence_less_than(next_sequence + 5, buffer.begin()->first) };
            if (pile_up_skip or burst_skip)
            {

                next_sequence = buffer.begin()->first;
                it = buffer.find(next_sequence);

            }
            if (it == buffer.end()) return false;

        }

        out = std::move(it->second);
        buffer.erase(it);
        ++next_sequence;

        // Adaptive playout: every m_pops_between_adapt successful pops,
        // recompute target threshold from the smoothed jitter and slide
        // the live threshold one step toward the target. The single-step
        // move is intentional — snapping straight to the target would
        // cause audible pumping every time jitter spikes. One step per
        // ~500 ms means the buffer drifts smoothly over a few seconds.
        if (m_adaptive)
        {

            ++m_pops_since_adapt;
            if (m_pops_since_adapt >= m_pops_between_adapt)
            {

                const size_t target { compute_target_threshold() };
                if      (target > playout_threshold) { ++playout_threshold; ++m_adaptation_count; }
                else if (target < playout_threshold) { --playout_threshold; ++m_adaptation_count; }
                m_pops_since_adapt = 0;

            }

        }

        return true;

    }

    size_t PacketJitterBuffer::compute_target_threshold() const noexcept
    {

        // 2x the smoothed jitter on top of a 20 ms (2-frame) base covers
        // about 95% of arrivals under a Gaussian-ish jitter distribution.
        // We don't need real statistics here — the bounds + the slow
        // adaptation rate paper over any modeling errors.
        constexpr double base_ms     { 20.0 };
        constexpr double frame_ms    { 10.0 };       // opus 10 ms frames
        constexpr double jitter_gain { 2.0 };

        const double jitter_ms { m_stats.jitter_ms() };
        const double target_ms { base_ms + jitter_gain * jitter_ms };
        const size_t target    { static_cast<size_t>(std::ceil(target_ms / frame_ms)) };
        return std::clamp(target, m_min_threshold, m_max_threshold);

    }

    bool PacketJitterBuffer::packet_sequence_less_than (std::uint16_t packet_a_sequence_number, std::uint16_t packet_b_sequence_number) const noexcept
    {

        return static_cast<int16_t>(packet_a_sequence_number - packet_b_sequence_number) < 0;

    }

}
