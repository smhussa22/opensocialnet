#ifndef PACKET_JITTER_BUFFER_HH
#define PACKET_JITTER_BUFFER_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <map>
#include <mutex>

// 3rd party headers

// project headers
#include "JitterStats.hh"
#include "Packet.hh"

namespace OpenSocialNet::Network
{

    class PacketJitterBuffer
    {

    public:

        PacketJitterBuffer() = default;
        ~PacketJitterBuffer() = default;

        PacketJitterBuffer(const PacketJitterBuffer&)            = delete;
        PacketJitterBuffer& operator=(const PacketJitterBuffer&) = delete;
        PacketJitterBuffer(PacketJitterBuffer&&)                 = delete;
        PacketJitterBuffer& operator=(PacketJitterBuffer&&)      = delete;


        // inserts packet keyed by sequence number, discards if too late
        bool push(Packet& packet) noexcept;

        // returns the next packet in order if ready, returns false if not enough buffered yet
        bool pop(Packet& out) noexcept;

        // returns how many packets are currently held
        size_t get_size() const noexcept;

        // returns true if a is "before" b, accounting for uint16_t wraparound
        bool packet_sequence_less_than (std::uint16_t packet_a_sequence_number, std::uint16_t packet_b_sequence_number) const noexcept;


        void set_playout_threshold(size_t threshold) noexcept { playout_threshold = threshold; }
        size_t size() const noexcept { return buffer.size(); }
        bool is_playing() const noexcept { return playing; }
        const JitterStats& stats() const noexcept { return m_stats; }


        // ---- adaptive playout (depth-arc) ----
        //
        // When adaptive mode is on, every `pops_between_adapt` successful
        // pops the buffer recomputes a target playout threshold from the
        // smoothed inter-arrival jitter (JitterStats::jitter_ms()) and
        // slides the live threshold one step toward that target. Off by
        // default to keep the existing fixed-threshold behaviour for
        // anyone who hasn't opted in.
        //
        // Math (see compute_target_threshold in the impl): target frames =
        //   clamp(ceil((20 ms base + 2.0 * jitter_ms) / 10 ms frame),
        //         m_min_threshold, m_max_threshold)
        // The 2x multiplier is a Gaussian-ish "cover ~95% of arrivals"
        // heuristic. Bounds default to [1, 20] (= 10..200 ms of audio).
        void   set_adaptive(bool enabled) noexcept { m_adaptive = enabled; }
        bool   adaptive() const noexcept           { return m_adaptive; }

        void   set_adaptive_bounds(size_t min_frames, size_t max_frames) noexcept
        {

            m_min_threshold = min_frames > 0 ? min_frames : 1;
            m_max_threshold = max_frames > m_min_threshold ? max_frames : m_min_threshold;

        }

        size_t current_playout_threshold() const noexcept { return playout_threshold; }
        size_t adaptation_count() const noexcept           { return m_adaptation_count; }


    private:

        // Recompute target threshold from JitterStats. Pure function of
        // jitter_ms() and the configured bounds; doesn't touch state.
        size_t compute_target_threshold() const noexcept;

        std::map<std::uint16_t, Packet> buffer            {};       // packets keyed by sequence number, autoordered
        std::uint16_t                   next_sequence     {};       // sequence number expected to play next
        size_t                          playout_threshold { 2 };    // minimum packets buffered before playback starts; mutated by the adaptive logic when m_adaptive is true
        bool                            playing           { false }; // true once there are enough packets to start
        mutable std::mutex              mutex             {};       // locks all operations and prevents push/pop races
        JitterStats                     m_stats           {};       // RFC 3550 inter-arrival jitter + loss / order tracking

        // Adaptive state. All mutated under `mutex` from inside pop().
        bool                            m_adaptive               { false }; // off by default for backward compat; flip via set_adaptive
        size_t                          m_min_threshold          { 1 };     // floor for adaptation (1 frame = 10 ms)
        size_t                          m_max_threshold          { 20 };    // ceiling (20 frames = 200 ms)
        size_t                          m_pops_since_adapt       { 0 };     // counts toward pops_between_adapt
        size_t                          m_pops_between_adapt     { 50 };    // recompute every ~500 ms at 100 fps
        size_t                          m_adaptation_count       { 0 };     // monotonic for [net-stats] / diagnostics

    };

}

#endif // PACKET_JITTER_BUFFER_HH
