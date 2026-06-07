#ifndef OSN_RELAY_ROOM_TABLE_HH
#define OSN_RELAY_ROOM_TABLE_HH

// related headers

// c sys headers
#include <netinet/in.h>

// cpp stdlib headers
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// 3rd party headers

// project headers

namespace OpenSocialNet::Relay
{

    // Last-seen + address tracking per peer. The relay updates `addr` on every
    // inbound packet — peers behind NAT can have their public mapping rebind,
    // so we always trust the latest address rather than the first we saw.
    struct PeerEntry
    {

        sockaddr_in addr {};                                           // peer's current public UDP endpoint
        std::chrono::steady_clock::time_point last_seen {};            // refreshed on every inbound packet for reaping

    };


    // Owns the room_id -> (peer_id -> PeerEntry) map that drives every fan-out
    // decision. Intentionally single-threaded; the v1 relay does recv + lookup
    // + sendto on one thread, so no mutex is needed. If we ever scale to a
    // SO_REUSEPORT worker pool, this becomes either sharded or wrapped in a
    // mutex — but not before there's a profiling-driven reason.
    class RoomTable
    {

    public:

        RoomTable() = default;

        RoomTable(const RoomTable&)            = delete;
        RoomTable& operator=(const RoomTable&) = delete;
        RoomTable(RoomTable&&)                 = delete;
        RoomTable& operator=(RoomTable&&)      = delete;


        // Record that a packet just arrived from this (room_id, peer_id, src)
        // tuple. Returns the list of every OTHER peer's current address in
        // this room so the caller can fan the packet out to each of them.
        // Updates the sender's PeerEntry in place — handles auto-join on first
        // packet and NAT rebind on subsequent ones.
        std::vector<sockaddr_in> on_packet(std::uint64_t room_id, std::uint32_t peer_id, const sockaddr_in& src, std::chrono::steady_clock::time_point now);

        // Drop any peer whose last_seen is older than `ttl` relative to `now`.
        // Empty rooms get erased too so the map size tracks reality. Returns
        // the number of peers dropped, mostly so the recv loop can log it.
        std::size_t reap(std::chrono::seconds ttl, std::chrono::steady_clock::time_point now);

        std::size_t room_count() const noexcept { return m_rooms.size(); }
        std::size_t peer_count() const noexcept;


    private:

        // room_id -> (peer_id -> PeerEntry). Outer map is small (≤ a few
        // hundred rooms at most for a single instance). Inner map is even
        // smaller (≤ 10 peers in our target).
        std::unordered_map<std::uint64_t, std::unordered_map<std::uint32_t, PeerEntry>> m_rooms {};

    };

}

#endif // OSN_RELAY_ROOM_TABLE_HH
