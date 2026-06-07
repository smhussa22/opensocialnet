// related headers
#include "RoomTable.hh"

// c sys headers

// cpp stdlib headers
#include <utility>

// 3rd party headers

// project headers

namespace OpenSocialNet::Relay
{

    std::vector<sockaddr_in> RoomTable::on_packet(std::uint64_t room_id, std::uint32_t peer_id, const sockaddr_in& src, std::chrono::steady_clock::time_point now)
    {

        auto& room { m_rooms[room_id] };

        // Insert or overwrite. Overwriting on every packet means we transparently
        // handle the case where a peer's NAT mapping rebinds mid-session — the
        // newest packet is always the source of truth for "where do I send to
        // reach this peer".
        room[peer_id] = PeerEntry { src, now };

        // Build the recipient list: every other peer currently in this room.
        // Vector copy is fine — rooms are tiny (≤10 peers) and the relay's
        // hot path is dominated by the sendto syscalls, not this loop.
        std::vector<sockaddr_in> recipients { };
        recipients.reserve(room.size());
        for (const auto& [pid, entry] : room)
        {

            if (pid == peer_id) continue;
            recipients.push_back(entry.addr);

        }

        return recipients;

    }

    std::size_t RoomTable::reap(std::chrono::seconds ttl, std::chrono::steady_clock::time_point now)
    {

        std::size_t dropped { 0 };

        // Erase-while-iterating idiom for both the inner peer map and the
        // outer room map (rooms that lose their last peer get erased so the
        // table doesn't accumulate ghost entries).
        for (auto room_it { m_rooms.begin() }; room_it != m_rooms.end(); )
        {

            auto& peers { room_it->second };
            for (auto peer_it { peers.begin() }; peer_it != peers.end(); )
            {

                if (now - peer_it->second.last_seen > ttl)
                {

                    peer_it = peers.erase(peer_it);
                    ++dropped;

                }
                else ++peer_it;

            }

            if (peers.empty()) room_it = m_rooms.erase(room_it);
            else ++room_it;

        }

        return dropped;

    }

    std::size_t RoomTable::peer_count() const noexcept
    {

        std::size_t n { 0 };
        for (const auto& [_, peers] : m_rooms) n += peers.size();
        return n;

    }

}
