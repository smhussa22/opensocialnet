#include "RoomRegistry.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    Room* RoomRegistry::get_or_create(std::string_view) noexcept
    {

        return nullptr;

    }

    Room* RoomRegistry::find(std::string_view) noexcept
    {

        return nullptr;

    }

    bool RoomRegistry::destroy_if_empty(std::string_view) noexcept
    {

        return false;

    }

    std::size_t RoomRegistry::room_count() const noexcept
    {

        std::lock_guard<std::mutex> lock { rooms_mutex };
        return rooms.size();

    }

}
