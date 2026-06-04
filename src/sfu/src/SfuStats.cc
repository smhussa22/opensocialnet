#include "SfuStats.hh"

// c sys headers
#include <cstdio>

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    void SfuStats::add_rtp_video_in(std::size_t bytes) noexcept
    {

        rtp_video_packets_in.fetch_add(1, std::memory_order_relaxed);
        rtp_video_bytes_in.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);

    }

    void SfuStats::add_rtp_video_out(std::size_t bytes) noexcept
    {

        rtp_video_packets_out.fetch_add(1, std::memory_order_relaxed);
        rtp_video_bytes_out.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);

    }

    void SfuStats::add_rtp_audio_in(std::size_t bytes) noexcept
    {

        rtp_audio_packets_in.fetch_add(1, std::memory_order_relaxed);
        rtp_audio_bytes_in.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);

    }

    void SfuStats::add_rtp_audio_out(std::size_t bytes) noexcept
    {

        rtp_audio_packets_out.fetch_add(1, std::memory_order_relaxed);
        rtp_audio_bytes_out.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);

    }

    void SfuStats::add_rtp_screen_in(std::size_t bytes) noexcept
    {

        rtp_screen_packets_in.fetch_add(1, std::memory_order_relaxed);
        rtp_screen_bytes_in.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);

    }

    void SfuStats::add_rtp_screen_out(std::size_t bytes) noexcept
    {

        rtp_screen_packets_out.fetch_add(1, std::memory_order_relaxed);
        rtp_screen_bytes_out.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);

    }

    void SfuStats::inc_active_peers() noexcept
    {

        active_peers.fetch_add(1, std::memory_order_relaxed);

    }

    void SfuStats::dec_active_peers() noexcept
    {

        active_peers.fetch_sub(1, std::memory_order_relaxed);

    }

    void SfuStats::inc_active_rooms() noexcept
    {

        active_rooms.fetch_add(1, std::memory_order_relaxed);

    }

    void SfuStats::dec_active_rooms() noexcept
    {

        active_rooms.fetch_sub(1, std::memory_order_relaxed);

    }

    SfuStats::Snapshot SfuStats::snapshot() const noexcept
    {

        Snapshot s { };
        s.rtp_video_packets_in = rtp_video_packets_in.load(std::memory_order_relaxed);
        s.rtp_video_packets_out = rtp_video_packets_out.load(std::memory_order_relaxed);
        s.rtp_audio_packets_in = rtp_audio_packets_in.load(std::memory_order_relaxed);
        s.rtp_audio_packets_out = rtp_audio_packets_out.load(std::memory_order_relaxed);
        s.rtp_screen_packets_in = rtp_screen_packets_in.load(std::memory_order_relaxed);
        s.rtp_screen_packets_out = rtp_screen_packets_out.load(std::memory_order_relaxed);
        s.rtp_video_bytes_in = rtp_video_bytes_in.load(std::memory_order_relaxed);
        s.rtp_video_bytes_out = rtp_video_bytes_out.load(std::memory_order_relaxed);
        s.rtp_audio_bytes_in = rtp_audio_bytes_in.load(std::memory_order_relaxed);
        s.rtp_audio_bytes_out = rtp_audio_bytes_out.load(std::memory_order_relaxed);
        s.rtp_screen_bytes_in = rtp_screen_bytes_in.load(std::memory_order_relaxed);
        s.rtp_screen_bytes_out = rtp_screen_bytes_out.load(std::memory_order_relaxed);
        s.active_peers = active_peers.load(std::memory_order_relaxed);
        s.active_rooms = active_rooms.load(std::memory_order_relaxed);
        return s;

    }

    void SfuStats::log_snapshot() const noexcept
    {

        // grab a non-transactional snapshot and dump it as one compact line.
        const Snapshot s { snapshot() };
        std::printf("[sfu-stats] rooms=%llu peers=%llu video_in=%llu/%lluB video_out=%llu/%lluB audio_in=%llu/%lluB audio_out=%llu/%lluB screen_in=%llu/%lluB screen_out=%llu/%lluB\n",
            static_cast<unsigned long long>(s.active_rooms),
            static_cast<unsigned long long>(s.active_peers),
            static_cast<unsigned long long>(s.rtp_video_packets_in),
            static_cast<unsigned long long>(s.rtp_video_bytes_in),
            static_cast<unsigned long long>(s.rtp_video_packets_out),
            static_cast<unsigned long long>(s.rtp_video_bytes_out),
            static_cast<unsigned long long>(s.rtp_audio_packets_in),
            static_cast<unsigned long long>(s.rtp_audio_bytes_in),
            static_cast<unsigned long long>(s.rtp_audio_packets_out),
            static_cast<unsigned long long>(s.rtp_audio_bytes_out),
            static_cast<unsigned long long>(s.rtp_screen_packets_in),
            static_cast<unsigned long long>(s.rtp_screen_bytes_in),
            static_cast<unsigned long long>(s.rtp_screen_packets_out),
            static_cast<unsigned long long>(s.rtp_screen_bytes_out));

    }

}
