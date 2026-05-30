#include "SfuStats.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    void SfuStats::note_peer_connected() noexcept
    {

        peers_connected_count.fetch_add(1, std::memory_order_relaxed);

    }

    void SfuStats::note_peer_disconnected() noexcept
    {

        peers_disconnected_count.fetch_add(1, std::memory_order_relaxed);

    }

    void SfuStats::note_room_created() noexcept
    {

        rooms_created_count.fetch_add(1, std::memory_order_relaxed);

    }

    void SfuStats::note_room_destroyed() noexcept
    {

        rooms_destroyed_count.fetch_add(1, std::memory_order_relaxed);

    }

    void SfuStats::note_packet_forwarded(std::size_t bytes) noexcept
    {

        packets_forwarded_count.fetch_add(1, std::memory_order_relaxed);
        bytes_forwarded_count.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);

    }

    void SfuStats::note_packet_dropped(std::size_t bytes) noexcept
    {

        packets_dropped_count.fetch_add(1, std::memory_order_relaxed);
        bytes_dropped_count.fetch_add(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);

    }

    SfuStats::Snapshot SfuStats::snapshot() const noexcept
    {

        Snapshot s { };
        s.peers_connected = peers_connected_count.load(std::memory_order_relaxed);
        s.peers_disconnected = peers_disconnected_count.load(std::memory_order_relaxed);
        s.rooms_created = rooms_created_count.load(std::memory_order_relaxed);
        s.rooms_destroyed = rooms_destroyed_count.load(std::memory_order_relaxed);
        s.packets_forwarded = packets_forwarded_count.load(std::memory_order_relaxed);
        s.bytes_forwarded = bytes_forwarded_count.load(std::memory_order_relaxed);
        s.packets_dropped = packets_dropped_count.load(std::memory_order_relaxed);
        s.bytes_dropped = bytes_dropped_count.load(std::memory_order_relaxed);
        return s;

    }

}
