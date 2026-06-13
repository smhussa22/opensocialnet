// related headers
#include "PeerVideoRouter.hh"

// c sys headers
#include <cstdio>

// cpp stdlib headers
#include <string>
#include <vector>

// 3rd party headers

// project headers

namespace
{

    std::uint64_t source_key(std::uint32_t peer_id, std::uint32_t ssrc) noexcept
    {

        return (static_cast<std::uint64_t>(peer_id) << 32) | ssrc;

    }

    // Timestamp skips longer than this many frame-steps are a sender
    // restart / discontinuity, not loss — resync instead of concealing
    // into the void.
    constexpr std::uint32_t maximum_missed_frame_steps { 30 };

}

namespace OpenSocialNet::Network
{

    PeerVideoRouter::PeerVideoRouter(Plc::VideoPlcStrategy plc_strategy, bool render_enabled) noexcept : m_plc_strategy { plc_strategy }, m_render_enabled { render_enabled }
    {

    }

    void PeerVideoRouter::route(Packet& packet)
    {

        const std::uint64_t key { source_key(packet.header.peer_id, packet.header.ssrc) };

        auto it = m_sources.find(key);
        if (it == m_sources.end())
        {

            auto source { std::make_unique<Source>(m_plc_strategy) };
            if (!source->decoder.valid())
            {

                std::printf("[vid-router] decoder init failed for peer=%08x ssrc=%u — dropping stream\n", packet.header.peer_id, packet.header.ssrc);
                return;

            }
            source->peer_id = packet.header.peer_id;
            source->ssrc = packet.header.ssrc;
            source->is_screen = packet.header.payload_type == PayloadType::H264_Screen;
            std::printf("[vid-router] new source peer=%08x ssrc=%u kind=%s\n", packet.header.peer_id, packet.header.ssrc, source->is_screen ? "screen" : "camera");
            it = m_sources.emplace(key, std::move(source)).first;

        }

        it->second->jitter_buffer.push(packet);
        it->second->last_packet = std::chrono::steady_clock::now();

    }

    std::size_t PeerVideoRouter::pump()
    {

        std::size_t presented { 0 };
        Packet packet {};

        for (auto& [key, source_ptr] : m_sources)
        {

            Source& source { *source_ptr };

            while (source.jitter_buffer.pop(packet))
            {

                const bool seq_contiguous { source.have_sequence && static_cast<std::uint16_t>(source.last_sequence + 1) == packet.header.sequence };
                source.last_sequence = packet.header.sequence;
                source.have_sequence = true;

                // Two flavours of loss surface at a frame boundary. A
                // partial: the previous frame never saw its marker, so the
                // reassembler is about to abandon it — conceal in its
                // place. And wholly vanished frames: the previous frame
                // completed cleanly but the timestamp skipped more than
                // one frame-step (the packetizer bumps timestamps per
                // encoded frame, never wall-clock, so a skip is always
                // loss) — conceal one substitute per vanished frame. The
                // frame-step itself is learned from sequence-contiguous
                // transitions, where the delta is exactly one step.
                const bool timestamp_changed { source.have_timestamp && packet.header.timestamp != source.current_timestamp };
                if (timestamp_changed)
                {

                    std::size_t to_conceal { 0 };
                    if (!source.frame_completed)
                    {

                        ++source.frames_dropped;
                        ++to_conceal;

                    }

                    const std::uint32_t delta { packet.header.timestamp - source.current_timestamp };
                    if (seq_contiguous) source.timestamp_step = delta;
                    else if (source.timestamp_step != 0)
                    {

                        const std::uint32_t steps { delta / source.timestamp_step };
                        if (steps > 1 && steps <= maximum_missed_frame_steps)
                        {

                            source.frames_missed += steps - 1;
                            to_conceal += steps - 1;

                        }

                    }

                    for (std::size_t i { 0 }; i < to_conceal; ++i) conceal_one(source);

                }
                if (!source.have_timestamp || timestamp_changed)
                {

                    source.current_timestamp = packet.header.timestamp;
                    source.have_timestamp = true;
                    source.frame_completed = false;

                }

                if (!source.reassembler.feed(packet)) continue;
                source.frame_completed = true;

                // decode_packet returning false is not necessarily loss —
                // the decoder may just be buffering its first frames — so
                // only real decode output feeds the PLC + renderer.
                std::uint8_t* planes[3] {};
                int strides[3] {};
                if (!source.decoder.decode_packet(source.reassembler.take_complete_frame(), planes, strides)) continue;

                ++source.frames_decoded;
                source.plc.on_real_frame(planes, strides, source.decoder.width(), source.decoder.height());
                present(source, planes, strides, source.decoder.width(), source.decoder.height());
                ++presented;

            }

        }

        return presented;

    }

    void PeerVideoRouter::conceal_one(Source& source)
    {

        std::uint8_t* planes[3] {};
        int strides[3] {};
        if (!source.plc.conceal(planes, strides)) return;

        present(source, planes, strides, source.plc.width(), source.plc.height());

    }

    void PeerVideoRouter::present(Source& source, const std::uint8_t* const planes[3], const int strides[3], int width, int height)
    {

        if (!m_render_enabled || source.renderer_dead) return;

        if (!source.renderer_started)
        {

            char title[64] {};
            std::snprintf(title, sizeof(title), "peer %08x %s (ssrc %u)", source.peer_id, source.is_screen ? "screen" : "camera", source.ssrc);
            if (!source.renderer.init(width, height, title))
            {

                std::printf("[vid-router] renderer init failed for peer=%08x ssrc=%u — decoding headless\n", source.peer_id, source.ssrc);
                source.renderer_dead = true;
                return;

            }
            source.renderer_started = true;

        }

        const std::uint8_t* render_planes[3] { planes[0], planes[1], planes[2] };
        if (source.renderer.render_frame(render_planes, strides, width, height)) ++source.frames_rendered;

    }

    std::size_t PeerVideoRouter::reap_idle(std::chrono::seconds ttl)
    {

        const auto now { std::chrono::steady_clock::now() };
        std::size_t reaped { 0 };

        for (auto it = m_sources.begin(); it != m_sources.end();)
        {

            if (now - it->second->last_packet > ttl)
            {

                std::printf("[vid-router] reaping idle source peer=%08x ssrc=%u\n", it->second->peer_id, it->second->ssrc);
                it = m_sources.erase(it);
                ++reaped;

            }
            else ++it;

        }

        return reaped;

    }

    VideoRouterStats PeerVideoRouter::stats() const
    {

        VideoRouterStats agg {};
        agg.sources = m_sources.size();

        for (const auto& [key, source_ptr] : m_sources)
        {

            const Source& source { *source_ptr };
            const auto snap { source.jitter_buffer.stats().snapshot() };
            agg.packets_observed += snap.packets_observed;
            agg.packets_lost += snap.packets_lost;
            agg.packets_out_of_order += snap.packets_out_of_order;
            agg.buffered_packets += source.jitter_buffer.size();
            agg.frames_decoded += source.frames_decoded;
            agg.frames_concealed += source.plc.concealments_emitted();
            agg.frames_dropped += source.frames_dropped;
            agg.frames_missed += source.frames_missed;
            agg.frames_rendered += source.frames_rendered;

        }

        return agg;

    }

    std::size_t PeerVideoRouter::source_count() const
    {

        return m_sources.size();

    }

}
