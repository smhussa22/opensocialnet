#include "Room.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    Room::Room(std::string id) noexcept : room_id { std::move(id) }
    {



    }

    bool Room::add_peer(std::shared_ptr<SfuPeer> peer) noexcept
    {

        if (!peer) return false;
        
        std::scoped_lock lock { peers_mutex };
        peers.push_back(std::move(peer));
        return true;

    }

    bool Room::remove_peer(std::string_view peer_id) noexcept
    {

        std::scoped_lock lock { peers_mutex };

        // find the first peer whos peer id matches the peer id we want to remove
        auto it { std::find_if(peers.begin(), peers.end(), [peer_id](const std::shared_ptr<SfuPeer>& p) { return p && p->peer_id() == peer_id; } ) };
        if (it == peers.end()) return false;

        peers.erase(it);
        return true;

    }

    void Room::forward_video_rtp(std::string_view source_peer_id, ::rtc::message_variant data) noexcept
    {

        // lock snapshot dont iterate when holding peers mutex since send_video_rtp doesnt net work io
        std::vector<std::shared_ptr<SfuPeer>> snapshot { };

        {

            std::scoped_lock lock { peers_mutex };
            snapshot = peers;

        }

        for (auto& peer : snapshot)
        {

            if (!peer) continue;
            if (peer->peer_id() == source_peer_id) continue;
            peer->send_video_rtp(data);

        }

    }

    void Room::forward_audio_rtp(std::string_view source_peer_id, ::rtc::message_variant data) noexcept
    {

        // lock snapshot dont iterate when holding peers mutex since send_video_rtp doesnt net work io
        std::vector<std::shared_ptr<SfuPeer>> snapshot { };

        {

            std::scoped_lock lock { peers_mutex };
            snapshot = peers;

        }

        for (auto& peer : snapshot)
        {

            if (!peer) continue;
            if (peer->peer_id() == source_peer_id) continue;
            peer->send_audio_rtp(data);

        }

    }

    std::string_view Room::id() const noexcept
    {

        return room_id;

    }

    std::size_t Room::peer_count() const noexcept
    {

        std::lock_guard<std::mutex> lock { peers_mutex };
        return peers.size();

    }

    bool Room::empty() const noexcept
    {

        std::lock_guard<std::mutex> lock { peers_mutex };
        return peers.empty();

    }

}
