// related headers
#include "VideoPacketizer.hh"

// c sys headers
#include <cstring>

// cpp stdlib headers

// 3rd party headers

// project headers
#include "NetworkConstants.hh"

namespace OpenSocialNet::Video
{

    void VideoPacketizer::init(std::uint32_t new_ssrc, std::uint32_t timestamp_step_per_frame, OpenSocialNet::Network::PayloadType new_payload_type) noexcept
    {

        ssrc = new_ssrc;
        sequence = 0;
        timestamp = 0;
        timestamp_step = timestamp_step_per_frame;
        payload_type = new_payload_type;

    }

    std::vector<OpenSocialNet::Network::Packet> VideoPacketizer::packetize_frame(std::span<const std::byte> h264_bytes) noexcept
    {

        std::vector<OpenSocialNet::Network::Packet> packets { };
        if (h264_bytes.empty()) return packets;

        const std::size_t chunk_size { OpenSocialNet::Network::maximum_packet_size };
        std::size_t offset { 0 };

        // chop the frame into chunk_size-sized payloads; last one carries marker=1
        while (offset < h264_bytes.size())
        {

            std::size_t remaining { h264_bytes.size() - offset };
            std::size_t this_chunk { remaining < chunk_size ? remaining : chunk_size };
            bool is_last { offset + this_chunk == h264_bytes.size() };

            OpenSocialNet::Network::Packet pkt { };
            pkt.header.ssrc = ssrc;
            pkt.header.timestamp = timestamp;
            pkt.header.sequence = sequence++;
            pkt.header.payload_size = static_cast<std::uint16_t>(this_chunk);
            pkt.header.payload_type = payload_type;
            pkt.header.flags        = is_last ? OpenSocialNet::Network::PacketFlag::marker : std::uint8_t { 0 };

            std::memcpy(pkt.payload, h264_bytes.data() + offset, this_chunk);

            packets.push_back(pkt);
            offset += this_chunk;

        }

        timestamp += timestamp_step;
        return packets;

    }

}
