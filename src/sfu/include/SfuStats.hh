#ifndef SFU_STATS_HH
#define SFU_STATS_HH

// related headers

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <atomic>

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    // lock-free SFU-wide counters bumped from any thread (the forwarding hot
    // path lives in a libdatachannel callback thread, plus gRPC worker threads
    // service RoomRegistry mutations), so every increment is an atomic relaxed
    // fetch_add — nothing waits on these values, they're pure counters used
    // for /stats endpoints, Kafka stats events, log_snapshot() and future
    // Prometheus exposition. snapshot() returns a non-transactional copy:
    // individual fields are eventually-consistent rather than locked.
    // addresses goals.md "Observability" + "Low-latency systems thinking".
    class SfuStats
    {

    public:
        // POD mirror of every counter, populated by snapshot(). consumers read
        // this struct without touching any atomics so they can format / log /
        // serialize freely.
        struct Snapshot
        {

            std::uint64_t rtp_video_packets_in { 0 }; // incoming video RTP from browsers
            std::uint64_t rtp_video_packets_out { 0 }; // outgoing video RTP fanned to browsers
            std::uint64_t rtp_audio_packets_in { 0 }; // incoming audio RTP from browsers
            std::uint64_t rtp_audio_packets_out { 0 }; // outgoing audio RTP fanned to browsers
            std::uint64_t rtp_screen_packets_in { 0 }; // incoming screen-share video RTP
            std::uint64_t rtp_screen_packets_out { 0 }; // outgoing screen-share video RTP
            std::uint64_t rtp_video_bytes_in { 0 }; // total bytes of incoming video RTP payloads
            std::uint64_t rtp_video_bytes_out { 0 }; // total bytes of outgoing video RTP payloads
            std::uint64_t rtp_audio_bytes_in { 0 }; // total bytes of incoming audio RTP payloads
            std::uint64_t rtp_audio_bytes_out { 0 }; // total bytes of outgoing audio RTP payloads
            std::uint64_t rtp_screen_bytes_in { 0 }; // total bytes of incoming screen RTP payloads
            std::uint64_t rtp_screen_bytes_out { 0 }; // total bytes of outgoing screen RTP payloads
            std::uint64_t active_peers { 0 }; // current peer count (gauge, inc/dec on AddPeer/RemovePeer)
            std::uint64_t active_rooms { 0 }; // current room count (gauge, inc/dec on RoomRegistry alloc/free)

        };

        SfuStats() noexcept = default;
        ~SfuStats() = default;

        SfuStats(const SfuStats&) = delete;
        SfuStats& operator=(const SfuStats&) = delete;
        SfuStats(SfuStats&&) = delete;
        SfuStats& operator=(SfuStats&&) = delete;

        // bumps rtp_video_packets_in by 1 and rtp_video_bytes_in by bytes.
        void add_rtp_video_in(std::size_t bytes) noexcept;

        // bumps rtp_video_packets_out by 1 and rtp_video_bytes_out by bytes.
        void add_rtp_video_out(std::size_t bytes) noexcept;

        // bumps rtp_audio_packets_in by 1 and rtp_audio_bytes_in by bytes.
        void add_rtp_audio_in(std::size_t bytes) noexcept;

        // bumps rtp_audio_packets_out by 1 and rtp_audio_bytes_out by bytes.
        void add_rtp_audio_out(std::size_t bytes) noexcept;

        // bumps rtp_screen_packets_in by 1 and rtp_screen_bytes_in by bytes.
        void add_rtp_screen_in(std::size_t bytes) noexcept;

        // bumps rtp_screen_packets_out by 1 and rtp_screen_bytes_out by bytes.
        void add_rtp_screen_out(std::size_t bytes) noexcept;

        // active_peers gauge — call from Room::add_peer / Room::remove_peer.
        void inc_active_peers() noexcept;
        void dec_active_peers() noexcept;

        // active_rooms gauge — call from RoomRegistry when a Room is alloc'd / freed.
        void inc_active_rooms() noexcept;
        void dec_active_rooms() noexcept;

        // returns a non-atomic snapshot. counters may move while we read them,
        // so individual fields are eventually-consistent rather than transactional.
        [[nodiscard]] Snapshot snapshot() const noexcept;

        // grabs snapshot() and prints it via std::printf as a single compact line.
        // TODO: switch to std::print / std::println once GCC 14+ / libstdc++14 is
        // available on the deploy box (Ubuntu 24.04 currently ships GCC 13 without
        // <print> — see the matching TODO in SfuGrpcService.cc).
        void log_snapshot() const noexcept;

    private:
        std::atomic<std::uint64_t> rtp_video_packets_in { 0 }; // incoming video RTP from browsers
        std::atomic<std::uint64_t> rtp_video_packets_out { 0 }; // outgoing video RTP fanned to browsers
        std::atomic<std::uint64_t> rtp_audio_packets_in { 0 }; // incoming audio RTP from browsers
        std::atomic<std::uint64_t> rtp_audio_packets_out { 0 }; // outgoing audio RTP fanned to browsers
        std::atomic<std::uint64_t> rtp_screen_packets_in { 0 }; // incoming screen-share video RTP
        std::atomic<std::uint64_t> rtp_screen_packets_out { 0 }; // outgoing screen-share video RTP
        std::atomic<std::uint64_t> rtp_video_bytes_in { 0 }; // total bytes of incoming video RTP payloads
        std::atomic<std::uint64_t> rtp_video_bytes_out { 0 }; // total bytes of outgoing video RTP payloads
        std::atomic<std::uint64_t> rtp_audio_bytes_in { 0 }; // total bytes of incoming audio RTP payloads
        std::atomic<std::uint64_t> rtp_audio_bytes_out { 0 }; // total bytes of outgoing audio RTP payloads
        std::atomic<std::uint64_t> rtp_screen_bytes_in { 0 }; // total bytes of incoming screen RTP payloads
        std::atomic<std::uint64_t> rtp_screen_bytes_out { 0 }; // total bytes of outgoing screen RTP payloads
        std::atomic<std::uint64_t> active_peers { 0 }; // current peer count (gauge)
        std::atomic<std::uint64_t> active_rooms { 0 }; // current room count (gauge)

    };

}

#endif // SFU_STATS_HH
