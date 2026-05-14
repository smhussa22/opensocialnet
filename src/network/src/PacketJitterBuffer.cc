// related headers
#include "PacketJitterBuffer.hh"

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <map>
#include <mutex>

// 3rd party headers

// project headers
#include "Packet.hh"

namespace OpenSocialNet::Network
{

    bool PacketJitterBuffer::push(Packet& packet) noexcept
    {

        std::scoped_lock<std::mutex> lock { mutex };
        if (playing && packet.header.sequence < next_sequence) return false;
        if (buffer.size() >= 50) return false;

        buffer.emplace(packet.header.sequence, packet);
        return true;

    }

    bool PacketJitterBuffer::pop(Packet& out) noexcept
    {

        std::scoped_lock<std::mutex> lock { mutex };

        if (!playing)
        {

            if (buffer.size() < playout_threshold) return false;
            playing = true;
            next_sequence = buffer.begin()->first;

        }

        auto it = buffer.find(next_sequence);
        if (it == buffer.end())
        {

            ++next_sequence;
            return false;

        }

        out = std::move(it->second);
        buffer.erase(it);
        ++next_sequence;
        return true;

    }

    size_t PacketJitterBuffer::size() const noexcept
    {

        std::scoped_lock<std::mutex> lock { mutex };
        return buffer.size();

    }

}