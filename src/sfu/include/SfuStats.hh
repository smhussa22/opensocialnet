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
    // path lives in a libdatachannel callback thread, so increments must be
    // atomic). snapshot() returns a consistent struct copy for /stats endpoints,
    // Kafka stats events, or unit tests. addressable from the goals.md
    // "Observability" + "Low-latency systems thinking" categories.
    class SfuStats
    {

    public:
        struct Snapshot
        {

            std::uint64_t peers_connected { 0 }; // running count of successful PeerConnection completions
            std::uint64_t peers_disconnected { 0 }; // count of clean or unclean PeerConnection terminations
            std::uint64_t rooms_created { 0 }; // RoomRegistry::get_or_create that actually allocated
            std::uint64_t rooms_destroyed { 0 }; // destroy_if_empty that actually freed
            std::uint64_t packets_forwarded { 0 }; // RTP packets handed to outgoing tracks
            std::uint64_t bytes_forwarded { 0 }; // sum of forwarded packet sizes
            std::uint64_t packets_dropped { 0 }; // packets discarded (queue full, bad sequence, etc.)
            std::uint64_t bytes_dropped { 0 }; // sum of dropped packet sizes

        };

        SfuStats() noexcept = default;
        ~SfuStats() = default;

        SfuStats(const SfuStats&) = delete;
        SfuStats& operator=(const SfuStats&) = delete;
        SfuStats(SfuStats&&) = delete;
        SfuStats& operator=(SfuStats&&) = delete;

        // bumps the connected-peer counter; call when a PeerConnection reaches Connected.
        void note_peer_connected() noexcept;

        // bumps the disconnected-peer counter; call on clean or unclean teardown.
        void note_peer_disconnected() noexcept;

        // bumps the room-created counter; call when RoomRegistry actually allocates a Room.
        void note_room_created() noexcept;

        // bumps the room-destroyed counter; call when destroy_if_empty frees a Room.
        void note_room_destroyed() noexcept;

        // records one successfully forwarded RTP packet and its size in bytes.
        void note_packet_forwarded(std::size_t bytes) noexcept;

        // records one dropped RTP packet and its size in bytes (queue full, malformed, ratelimited, etc.).
        void note_packet_dropped(std::size_t bytes) noexcept;

        // returns a non-atomic snapshot. counters may move while we read them, so
        // individual fields are eventually-consistent rather than transactional.
        [[nodiscard]] Snapshot snapshot() const noexcept;

    private:
        std::atomic<std::uint64_t> peers_connected_count { 0 }; // total PeerConnections that reached Connected
        std::atomic<std::uint64_t> peers_disconnected_count { 0 }; // total PeerConnections that have terminated
        std::atomic<std::uint64_t> rooms_created_count { 0 }; // total Room allocations through RoomRegistry
        std::atomic<std::uint64_t> rooms_destroyed_count { 0 }; // total Room frees through RoomRegistry
        std::atomic<std::uint64_t> packets_forwarded_count { 0 }; // RTP packets successfully written to an outbound track
        std::atomic<std::uint64_t> bytes_forwarded_count { 0 }; // sum of sizes of forwarded RTP packets
        std::atomic<std::uint64_t> packets_dropped_count { 0 }; // RTP packets discarded before forwarding
        std::atomic<std::uint64_t> bytes_dropped_count { 0 }; // sum of sizes of dropped RTP packets

    };

}

#endif // SFU_STATS_HH
