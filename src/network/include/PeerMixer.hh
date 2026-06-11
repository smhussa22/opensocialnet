#ifndef PEER_MIXER_HH
#define PEER_MIXER_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

// 3rd party headers

// project headers
#include "AudioDecoder.hh"
#include "AudioPlc.hh"
#include "Packet.hh"
#include "PacketJitterBuffer.hh"

namespace OpenSocialNet::Network
{

    // Aggregate of every active source's counters, summed (or maxed where
    // summing makes no sense) for the single [net-stats] line.
    struct MixerStats
    {

        std::uint64_t packets_observed     { 0 };   // sum of JitterStats packets_observed across sources
        std::uint64_t packets_lost         { 0 };   // sum of gap-detected losses across sources
        std::uint64_t packets_out_of_order { 0 };   // sum of out-of-order arrivals across sources
        double        max_jitter_ms        { 0.0 }; // worst RFC 3550 jitter among sources (the binding constraint)
        std::size_t   buffered_packets     { 0 };   // sum of packets currently parked in all jitter buffers
        std::size_t   max_playout_threshold { 0 };  // deepest adaptive playout threshold among sources
        std::uint64_t adaptations          { 0 };   // sum of adaptive threshold moves across sources
        std::uint64_t concealments         { 0 };   // sum of PLC frames emitted across sources
        std::size_t   sources              { 0 };   // live source count (peers actively sending)

    };


    // The multi-peer receive spine: every inbound Packet is demuxed by
    // (peer_id, ssrc) into that source's own PacketJitterBuffer +
    // AudioDecoder + AudioPlc — opus decoder state is per-stream, and
    // sequence numbers from different senders are independent counters,
    // so sharing one buffer across peers would interleave-corrupt both.
    // Decoded frames from all sources are summed into one PCM frame
    // (naive mix, clamped) that feeds the single SDL output stream.
    //
    // Threading: route() / mix_one_frame() / reap_idle() run on the SDL
    // audio callback + main loop, stats() on the main loop — the map
    // itself is guarded by m_mu (the per-source jitter buffers have their
    // own internal lock, but creation/erasure of sources does not).
    class PeerMixer
    {

    public:

        explicit PeerMixer(Plc::PlcStrategy plc_strategy, bool adaptive_jitter) noexcept;

        PeerMixer(const PeerMixer&)            = delete;
        PeerMixer& operator=(const PeerMixer&) = delete;
        PeerMixer(PeerMixer&&)                 = delete;
        PeerMixer& operator=(PeerMixer&&)      = delete;


        // Push one inbound packet into its source's jitter buffer,
        // creating the source on first sight (auto-join, mirroring the
        // relay's RoomTable behaviour). Refreshes the source's last-seen
        // time for reap_idle.
        void route(Packet& packet);

        // Produce one mixed frame: for each source, pop+decode a real
        // frame or ask its PLC to conceal, then sum into `out` and clamp.
        // Returns the number of sources that contributed; 0 means the
        // caller should let SDL silence-fill instead.
        std::size_t mix_one_frame(std::span<float> out);

        // Drop sources that haven't sent anything for `ttl` — their peer
        // left the room (or died); freeing the slot also frees the opus
        // decoder. Returns how many were dropped so the caller can log.
        std::size_t reap_idle(std::chrono::seconds ttl);

        // Aggregate counters across all live sources for [net-stats].
        MixerStats stats() const;

        std::size_t source_count() const;


    private:

        // Everything one remote sender needs on the receive side. Heap
        // allocated because PacketJitterBuffer / AudioPlc are pinned
        // (non-movable) types.
        struct Source
        {

            PacketJitterBuffer                    jitter_buffer { };  // per-source reorder + playout depth
            Audio::AudioDecoder                   decoder       { };  // per-stream opus state (history feeds its PLC)
            Plc::AudioPlc                         plc;                // gap concealment bound to this source's decoder
            std::chrono::steady_clock::time_point last_packet   { }; // refreshed per packet; drives reap_idle

            Source(Plc::PlcStrategy strategy, bool adaptive) noexcept : plc { strategy, decoder }
            {

                if (adaptive) jitter_buffer.set_adaptive(true);

            }

        };

        Plc::PlcStrategy   m_plc_strategy    { Plc::PlcStrategy::Opus }; // strategy stamped into every new source's AudioPlc
        bool               m_adaptive_jitter { false };                  // forwarded to every new source's jitter buffer
        mutable std::mutex m_mu              { };                        // guards m_sources create/erase/iterate

        // (peer_id << 32 | ssrc) -> per-source receive state. ≤10 peers
        // by design, so an unordered_map walk per mixed frame is cheap.
        std::unordered_map<std::uint64_t, std::unique_ptr<Source>> m_sources { };

    };

}

#endif // PEER_MIXER_HH
