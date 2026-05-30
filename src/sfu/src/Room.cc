#include "Room.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    Room::Room(std::string id) noexcept
        : room_id { std::move(id) }
    {



    }

    bool Room::add_peer(std::shared_ptr<SfuPeer> peer) noexcept
    {

        (void)peer;
        return false;

    }

    bool Room::remove_peer(std::string_view peer_id) noexcept
    {

        (void)peer_id;
        return false;

    }

    void Room::forward_video_rtp(std::string_view source_peer_id, std::span<const std::byte> rtp_bytes) noexcept
    {

        (void)source_peer_id;
        (void)rtp_bytes;

    }

    void Room::forward_audio_rtp(std::string_view source_peer_id, std::span<const std::byte> rtp_bytes) noexcept
    {

        (void)source_peer_id;
        (void)rtp_bytes;

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
