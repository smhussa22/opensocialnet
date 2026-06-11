// related headers
#include "PeerMixer.hh"

// c sys headers
#include <cstdio>

// cpp stdlib headers
#include <algorithm>
#include <array>

// 3rd party headers

// project headers
#include "AudioConstants.hh"

namespace OpenSocialNet::Network
{

    namespace
    {

        std::uint64_t source_key(std::uint32_t peer_id, std::uint32_t ssrc) noexcept
        {

            return (static_cast<std::uint64_t>(peer_id) << 32) | ssrc;

        }

    }


    PeerMixer::PeerMixer(Plc::PlcStrategy plc_strategy, bool adaptive_jitter) noexcept : m_plc_strategy { plc_strategy }, m_adaptive_jitter { adaptive_jitter }
    {

    }


    void PeerMixer::route(Packet& packet)
    {

        const std::uint64_t key { source_key(packet.header.peer_id, packet.header.ssrc) };

        std::scoped_lock<std::mutex> lock { m_mu };

        auto it { m_sources.find(key) };
        if (it == m_sources.end())
        {

            auto source { std::make_unique<Source>(m_plc_strategy, m_adaptive_jitter) };
            if (!source->decoder.valid())
            {

                std::fprintf(stderr, "[mixer] opus decoder init failed for peer=%08x ssrc=%u — dropping source\n", packet.header.peer_id, packet.header.ssrc);
                return;

            }
            std::printf("[mixer] new source peer=%08x ssrc=%u (sources=%zu)\n", packet.header.peer_id, packet.header.ssrc, m_sources.size() + 1);
            it = m_sources.emplace(key, std::move(source)).first;

        }

        it->second->jitter_buffer.push(packet);
        it->second->last_packet = std::chrono::steady_clock::now();

    }


    std::size_t PeerMixer::mix_one_frame(std::span<float> out)
    {

        std::fill(out.begin(), out.end(), 0.0f);

        std::array<float, Audio::samples_per_frame> source_pcm { };
        std::size_t contributors { 0 };

        std::scoped_lock<std::mutex> lock { m_mu };

        for (auto& [key, source] : m_sources)
        {

            // Real frame if the jitter buffer has one ready; otherwise let
            // this source's PLC extrapolate from its decoder history. PLC
            // gives up past its consecutive cap (returns 0) so a silent
            // source costs nothing after ~50 ms of gap.
            Packet packet { };
            int samples { 0 };
            if (source->jitter_buffer.pop(packet))
            {

                samples = source->decoder.decode_bytes(std::span<const std::byte> { reinterpret_cast<const std::byte*>(packet.payload), packet.header.payload_size }, std::span<float> { source_pcm });
                if (samples > 0) source->plc.on_real_frame(std::span<const float> { source_pcm.data(), static_cast<std::size_t>(samples) });

            }
            else
            {

                samples = source->plc.conceal(std::span<float> { source_pcm });

            }

            if (samples <= 0) continue;

            const std::size_t mix_count { std::min(out.size(), static_cast<std::size_t>(samples)) };
            for (std::size_t i { 0 }; i < mix_count; ++i) out[i] += source_pcm[i];
            ++contributors;

        }

        // Summed streams can exceed [-1, 1]; a single source can't.
        if (contributors > 1) for (auto& sample : out) sample = std::clamp(sample, -1.0f, 1.0f);

        return contributors;

    }


    std::size_t PeerMixer::reap_idle(std::chrono::seconds ttl)
    {

        const auto now { std::chrono::steady_clock::now() };

        std::scoped_lock<std::mutex> lock { m_mu };

        std::size_t reaped { 0 };
        for (auto it { m_sources.begin() }; it != m_sources.end(); )
        {

            if (now - it->second->last_packet > ttl)
            {

                std::printf("[mixer] reaped idle source key=%016llx\n", static_cast<unsigned long long>(it->first));
                it = m_sources.erase(it);
                ++reaped;

            }
            else
            {

                ++it;

            }

        }
        return reaped;

    }


    MixerStats PeerMixer::stats() const
    {

        MixerStats agg { };

        std::scoped_lock<std::mutex> lock { m_mu };

        agg.sources = m_sources.size();
        for (const auto& [key, source] : m_sources)
        {

            const auto snap { source->jitter_buffer.stats().snapshot() };
            agg.packets_observed      += snap.packets_observed;
            agg.packets_lost          += snap.packets_lost;
            agg.packets_out_of_order  += snap.packets_out_of_order;
            agg.max_jitter_ms          = std::max(agg.max_jitter_ms, snap.jitter_ms);
            agg.buffered_packets      += source->jitter_buffer.size();
            agg.max_playout_threshold  = std::max(agg.max_playout_threshold, source->jitter_buffer.current_playout_threshold());
            agg.adaptations           += source->jitter_buffer.adaptation_count();
            agg.concealments          += source->plc.concealments_emitted();

        }
        return agg;

    }


    std::size_t PeerMixer::source_count() const
    {

        std::scoped_lock<std::mutex> lock { m_mu };
        return m_sources.size();

    }

}
