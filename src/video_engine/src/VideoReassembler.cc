// related headers
#include "VideoReassembler.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Video
{

    bool VideoReassembler::feed(const OpenSocialNet::Network::Packet& packet) noexcept
    {

        // a new packet arrives -> previous take_complete_frame() output is no longer guaranteed valid
        if (frame_ready)
        {

            frame_buffer.clear();
            frame_ready = false;

        }

        // timestamp change without having seen marker=1 means we lost the tail of the previous frame
        if (have_timestamp && packet.header.timestamp != current_timestamp) frame_buffer.clear();

        current_timestamp = packet.header.timestamp;
        have_timestamp = true;

        // append this packet's payload to the in-progress frame buffer
        const std::byte* payload_start { reinterpret_cast<const std::byte*>(packet.payload) };
        frame_buffer.insert(frame_buffer.end(), payload_start, payload_start + packet.header.payload_size);

        if ((packet.header.flags & OpenSocialNet::Network::PacketFlag::marker) != 0)
        {

            frame_ready = true;
            return true;

        }

        return false;

    }

    std::span<const std::byte> VideoReassembler::take_complete_frame() const noexcept
    {

        if (!frame_ready) return { };
        return { frame_buffer.data(), frame_buffer.size() };

    }

}
