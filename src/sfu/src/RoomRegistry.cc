#include "RoomRegistry.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    RoomRegistry::RoomRegistry(SfuStats& stats) noexcept : m_stats { stats }
    {



    }

    Room* RoomRegistry::get_or_create(std::string_view room_id) noexcept
    {

        std::scoped_lock lock { rooms_mutex };

        auto it { rooms.find(std::string { room_id }) };
        if (it != rooms.end()) return it->second.get();

        auto inserted { rooms.emplace(std::string { room_id }, std::make_unique<Room>(std::string { room_id }, m_stats)) };
        m_stats.inc_active_rooms();
        return inserted.first->second.get();

    }

    Room* RoomRegistry::find(std::string_view room_id) noexcept
    {

        std::scoped_lock lock { rooms_mutex };

        auto it { rooms.find(std::string { room_id }) };
        if (it == rooms.end()) return nullptr;
        return it->second.get();

    }

    bool RoomRegistry::destroy_if_empty(std::string_view room_id) noexcept
    {

        std::scoped_lock lock { rooms_mutex };

        auto it { rooms.find(std::string { room_id }) };
        if (it == rooms.end()) return false;
        if (!it->second->empty()) return false;

        rooms.erase(it);
        m_stats.dec_active_rooms();
        return true;

    }

    std::size_t RoomRegistry::room_count() const noexcept
    {

        std::scoped_lock lock { rooms_mutex };
        return rooms.size();

    }

}
