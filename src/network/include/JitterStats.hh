#ifndef JITTER_STATS_HH
#define JITTER_STATS_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <cmath>

// 3rd party headers

// project headers
#include "Packet.hh"

namespace OpenSocialNet::Network
{

    // RFC 3550 inter-arrival jitter + loss/order tracking.
    //
    // Unit assumptions:
    //   - now_us (arrival clock) is in microseconds (std::int64_t, steady_clock).
    //   - pkt.header.timestamp is in media-clock samples; for the D(i, j)
    //     calculation we treat it as microseconds-equivalent ticks. Callers
    //     wanting strict RFC 3550 semantics should ensure timestamps are in the
    //     same unit as arrival deltas, otherwise the jitter is a relative
    //     stability metric rather than wall-time accurate.
    //   - jitter is held internally in microseconds (double) and exposed in
    //     milliseconds via jitter_ms().
    class JitterStats
    {

    public:

        struct Snapshot
        {

            std::uint64_t packets_observed { 0 };        // total packets seen
            std::uint64_t packets_lost { 0 };            // estimated lost from seq gaps
            std::uint64_t packets_out_of_order { 0 };    // seq < last_seq events
            double jitter_ms { 0.0 };                    // smoothed RFC 3550 jitter, ms

        };

        JitterStats() = default;
        ~JitterStats() = default;

        JitterStats(const JitterStats&) = delete;
        JitterStats& operator=(const JitterStats&) = delete;
        JitterStats(JitterStats&&) = delete;
        JitterStats& operator=(JitterStats&&) = delete;

        // called per arrived packet; updates all tracked fields
        void observe(const Packet& pkt) noexcept
        {

            // grab arrival time in microseconds on the steady (monotonic) clock
            const std::int64_t now_us { std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() };
            const std::uint32_t seq { pkt.header.sequence };
            const std::uint32_t ts { pkt.header.timestamp };

            // first-ever observation: just seed the trackers, nothing to diff against
            if (!m_initialized.load(std::memory_order_acquire))
            {

                m_last_arrival_us.store(now_us, std::memory_order_relaxed);
                m_last_packet_timestamp.store(ts, std::memory_order_relaxed);
                m_last_sequence.store(seq, std::memory_order_relaxed);
                m_packets_observed.fetch_add(1, std::memory_order_relaxed);
                m_initialized.store(true, std::memory_order_release);
                return;

            }

            const std::int64_t prev_arrival_us { m_last_arrival_us.load(std::memory_order_relaxed) };
            const std::uint32_t prev_ts { m_last_packet_timestamp.load(std::memory_order_relaxed) };
            const std::uint32_t prev_seq { m_last_sequence.load(std::memory_order_relaxed) };

            // RFC 3550 D(i, j) = (arrival_j - arrival_i) - (ts_j - ts_i)
            const std::int64_t arrival_delta { now_us - prev_arrival_us };
            const std::int64_t ts_delta { static_cast<std::int64_t>(static_cast<std::int32_t>(ts - prev_ts)) };
            const double D { static_cast<double>(arrival_delta - ts_delta) };

            // J += (|D| - J) / 16, lock-free CAS loop on the atomic double
            double current { m_jitter_us.load(std::memory_order_relaxed) };
            double updated { 0.0 };
            do
            {

                updated = current + (std::fabs(D) - current) / 16.0;

            }
            while (!m_jitter_us.compare_exchange_weak(current, updated, std::memory_order_relaxed, std::memory_order_relaxed));

            // loss + out-of-order accounting (sequence is uint16 in the packet, treat gap with int16 wraparound)
            const std::int16_t seq_diff { static_cast<std::int16_t>(static_cast<std::uint16_t>(seq) - static_cast<std::uint16_t>(prev_seq)) };
            if (seq_diff > 1) m_packets_lost.fetch_add(static_cast<std::uint64_t>(seq_diff - 1), std::memory_order_relaxed);
            if (seq_diff < 0) m_packets_out_of_order.fetch_add(1, std::memory_order_relaxed);

            // refresh trackers + bump observed
            m_last_arrival_us.store(now_us, std::memory_order_relaxed);
            m_last_packet_timestamp.store(ts, std::memory_order_relaxed);
            m_last_sequence.store(seq, std::memory_order_relaxed);
            m_packets_observed.fetch_add(1, std::memory_order_relaxed);

        }

        void reset() noexcept
        {

            m_packets_observed.store(0, std::memory_order_relaxed);
            m_packets_lost.store(0, std::memory_order_relaxed);
            m_packets_out_of_order.store(0, std::memory_order_relaxed);
            m_jitter_us.store(0.0, std::memory_order_relaxed);
            m_last_arrival_us.store(0, std::memory_order_relaxed);
            m_last_packet_timestamp.store(0, std::memory_order_relaxed);
            m_last_sequence.store(0, std::memory_order_relaxed);
            m_initialized.store(false, std::memory_order_release);

        }

        std::uint64_t packets_observed() const noexcept { return m_packets_observed.load(std::memory_order_relaxed); }
        std::uint64_t packets_lost() const noexcept { return m_packets_lost.load(std::memory_order_relaxed); }
        std::uint64_t packets_out_of_order() const noexcept { return m_packets_out_of_order.load(std::memory_order_relaxed); }
        double jitter_ms() const noexcept { return m_jitter_us.load(std::memory_order_relaxed) / 1000.0; }
        std::int64_t last_arrival_us() const noexcept { return m_last_arrival_us.load(std::memory_order_relaxed); }
        std::uint32_t last_packet_timestamp() const noexcept { return m_last_packet_timestamp.load(std::memory_order_relaxed); }
        std::uint32_t last_sequence() const noexcept { return m_last_sequence.load(std::memory_order_relaxed); }

        Snapshot snapshot() const noexcept
        {

            Snapshot s {};
            s.packets_observed = packets_observed();
            s.packets_lost = packets_lost();
            s.packets_out_of_order = packets_out_of_order();
            s.jitter_ms = jitter_ms();
            return s;

        }

    private:

        std::atomic<std::uint64_t> m_packets_observed { 0 };       // total packets seen since construction/reset
        std::atomic<std::uint64_t> m_packets_lost { 0 };           // estimated loss inferred from seq gaps
        std::atomic<std::uint64_t> m_packets_out_of_order { 0 };   // count of packets where seq < prev seq
        std::atomic<double> m_jitter_us { 0.0 };                   // smoothed RFC 3550 jitter, microseconds
        std::atomic<std::int64_t> m_last_arrival_us { 0 };         // steady_clock micros of previous packet arrival
        std::atomic<std::uint32_t> m_last_packet_timestamp { 0 };  // header.timestamp of previous packet
        std::atomic<std::uint32_t> m_last_sequence { 0 };          // header.sequence of previous packet (widened from uint16)
        std::atomic<bool> m_initialized { false };                 // false until the first packet is observed

    };

}

#endif // JITTER_STATS_HH
