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

        m_stats.observe(packet);

        std::scoped_lock<std::mutex> lock { mutex };
        if (playing && packet_sequence_less_than(packet.header.sequence, next_sequence)) return false;
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
            // only skip if we have newer packets waiting — don't skip just because it's missing yet
            if (!buffer.empty() and packet_sequence_less_than(next_sequence + 5, buffer.begin()->first))
            {
                // gap is large enough that packet is truly lost, not just late
                next_sequence = buffer.begin()->first;
            }
            return false;
        }

        out = std::move(it->second);
        buffer.erase(it);
        ++next_sequence;
        return true;

    }

    bool PacketJitterBuffer::packet_sequence_less_than (std::uint16_t packet_a_sequence_number, std::uint16_t packet_b_sequence_number) const noexcept
    {

        return static_cast<int16_t>(packet_a_sequence_number - packet_b_sequence_number) < 0;

    }

}