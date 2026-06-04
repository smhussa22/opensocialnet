#include "Room.hh"

// c sys headers

// cpp stdlib headers
#include <variant>

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    namespace
    {

        // RTP payload size in bytes; works for either alternative of
        // rtc::message_variant (binary = vector<byte>, string).
        std::size_t message_size(const ::rtc::message_variant& msg) noexcept
        {

            return std::visit([](auto&& v) -> std::size_t { return v.size(); }, msg);

        }

    }

    Room::Room(std::string id, SfuStats& stats) noexcept : room_id { std::move(id) }, m_stats { stats }
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

        const std::size_t bytes { message_size(data) };
        m_stats.add_rtp_video_in(bytes);

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
            m_stats.add_rtp_video_out(bytes);

        }

    }

    void Room::forward_audio_rtp(std::string_view source_peer_id, ::rtc::message_variant data) noexcept
    {

        const std::size_t bytes { message_size(data) };
        m_stats.add_rtp_audio_in(bytes);

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
            m_stats.add_rtp_audio_out(bytes);

        }

    }

    void Room::forward_screen_video_rtp(std::string_view source_peer_id, ::rtc::message_variant data) noexcept
    {

        const std::size_t bytes { message_size(data) };
        m_stats.add_rtp_screen_in(bytes);

        // snapshot first; libdatachannel send() runs without holding peers_mutex
        std::vector<std::shared_ptr<SfuPeer>> snapshot { };

        {

            std::scoped_lock lock { peers_mutex };
            snapshot = peers;

        }

        for (auto& peer : snapshot)
        {

            if (!peer) continue;
            if (peer->peer_id() == source_peer_id) continue;
            if (!peer->has_screen_track()) continue; // skip consumers without renegotiated screen track
            peer->send_screen_video_rtp(data);
            m_stats.add_rtp_screen_out(bytes);

        }

    }

    std::vector<std::shared_ptr<SfuPeer>> Room::snapshot_peers() const noexcept
    {

        std::scoped_lock lock { peers_mutex };
        return peers;

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
