#ifndef SFU_ROOM_REGISTRY_HH
#define SFU_ROOM_REGISTRY_HH

// related headers
#include "Room.hh"
#include "SfuStats.hh"

// c sys headers
#include <cstddef>

// cpp stdlib headers
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    // global directory of active rooms keyed by room id. owns each Room as a
    // unique_ptr; lifetime ends when the registry destroys the entry (typically
    // when the last peer leaves). thread-safe — every call takes a shared mutex
    // around the map. signaling_server creates / destroys rooms via gRPC calls
    // that ultimately land in get_or_create / destroy_if_empty here.
    class RoomRegistry
    {

    public:
        // stats is a borrowed reference (owned by main()); it is passed into
        // every Room we allocate AND bumped here on inc/dec_active_rooms so
        // the room gauge tracks allocation lifetime.
        explicit RoomRegistry(SfuStats& stats) noexcept;
        ~RoomRegistry() = default;

        RoomRegistry(const RoomRegistry&) = delete;
        RoomRegistry& operator=(const RoomRegistry&) = delete;
        RoomRegistry(RoomRegistry&&) = delete;
        RoomRegistry& operator=(RoomRegistry&&) = delete;

        // returns existing room or creates one. the pointer is valid until
        // destroy_if_empty removes the entry. the registry retains ownership.
        Room* get_or_create(std::string_view room_id) noexcept;

        // returns the room if present, else nullptr. does not create.
        Room* find(std::string_view room_id) noexcept;

        // removes the room iff peer_count() == 0. returns true if it was removed.
        bool destroy_if_empty(std::string_view room_id) noexcept;

        // returns the number of rooms currently held by the registry; observability use.
        [[nodiscard]] std::size_t room_count() const noexcept;

    private:
        SfuStats& m_stats; // bumped on alloc / free; also threaded into each Room
        mutable std::mutex rooms_mutex { }; // guards rooms map across get/destroy
        std::unordered_map<std::string, std::unique_ptr<Room>> rooms { }; // owned rooms keyed by id

    };

}

#endif // SFU_ROOM_REGISTRY_HH
