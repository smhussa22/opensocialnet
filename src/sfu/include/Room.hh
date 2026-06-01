#ifndef SFU_ROOM_HH
#define SFU_ROOM_HH

// related headers
#include "SfuPeer.hh"

// c sys headers

// cpp stdlib headers
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

// 3rd party headers
#include <rtc/rtc.hpp>

// project headers

namespace OpenSocialNet::Sfu
{

    // a single logical voice/video channel. owns the peers currently inside and
    // drives the per-packet forwarding decisions (when peer A's video arrives,
    // send it to every other peer in the room). Layer 3 puts real forwarding in
    // here; Layer 1 just maintains the membership list.
    class Room
    {

    public:
        // constructs an empty room with the given identifier.
        explicit Room(std::string room_id) noexcept;
        ~Room() = default;

        Room(const Room&) = delete;
        Room& operator=(const Room&) = delete;
        Room(Room&&) = delete;
        Room& operator=(Room&&) = delete;

        // inserts peer into membership and wires its incoming tracks into the forwarder.
        // peer's id is set by the caller (RoomRegistry / signaling) before this call.
        bool add_peer(std::shared_ptr<SfuPeer> peer) noexcept;

        // detaches peer from membership and unwires its forwarding edges.
        bool remove_peer(std::string_view peer_id) noexcept;

        // dispatches an inbound video RTP packet from source_peer_id to every other peer.
        // hot path; will live in a dedicated forwarding thread once Layer 3 lands.
        void forward_video_rtp(std::string_view source_peer_id, ::rtc::message_variant data) noexcept;

        // same as forward_video_rtp but for audio. cheaper (smaller packets, higher rate).
        void forward_audio_rtp(std::string_view source_peer_id, ::rtc::message_variant data) noexcept;

        // returns this room's identifier (the key it lives under in RoomRegistry).
        [[nodiscard]] std::string_view id() const noexcept;

        // returns the current participant count; cheap, takes the peers mutex briefly.
        [[nodiscard]] std::size_t peer_count() const noexcept;

        // true when there are no peers — RoomRegistry uses this to decide when to free the Room.
        [[nodiscard]] bool empty() const noexcept;

    private:
        std::string room_id { }; // human-readable channel identifier
        mutable std::mutex peers_mutex { }; // guards peers vector against concurrent add/remove during forwarding
        std::vector<std::shared_ptr<SfuPeer>> peers { }; // current participants

    };

}

#endif // SFU_ROOM_HH
