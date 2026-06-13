#ifndef VIDEO_PACKETIZER_HH
#define VIDEO_PACKETIZER_HH

// related headers

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <span>
#include <vector>

// 3rd party headers

// project headers
#include "Packet.hh"

namespace OpenSocialNet::Video
{

    // Splits a frame's H.264 bytes into one or more network packets.
    // Owns sequence counter and per-frame timestamp; caller passes ssrc once.
    class VideoPacketizer
    {

    public:
        VideoPacketizer() noexcept = default;
        ~VideoPacketizer() = default;

        VideoPacketizer(const VideoPacketizer&) = delete;
        VideoPacketizer& operator=(const VideoPacketizer&) = delete;
        VideoPacketizer(VideoPacketizer&&) = default;
        VideoPacketizer& operator=(VideoPacketizer&&) = default;

        // Stores ssrc + the payload kind stamped on every packet (camera vs screen lane) and resets sequence/timestamp counters. Must be called before packetize_frame.
        void init(std::uint32_t ssrc, std::uint32_t timestamp_step_per_frame = 3000, OpenSocialNet::Network::PayloadType payload_type = OpenSocialNet::Network::PayloadType::H264) noexcept;

        // Splits one H.264 frame into packets. marker=1 only on the last packet.
        // All packets in a frame share the same timestamp; sequence increments per packet.
        std::vector<OpenSocialNet::Network::Packet> packetize_frame(std::span<const std::byte> h264_bytes) noexcept;

    private:
        std::uint32_t ssrc { 0 };               // sender stream ID, set once at init
        std::uint16_t sequence { 0 };           // monotonically increments per packet, wraps at 65535
        std::uint32_t timestamp { 0 };          // current frame's media timestamp
        std::uint32_t timestamp_step { 3000 };  // bump applied per frame (e.g., 90kHz / 30fps)
        OpenSocialNet::Network::PayloadType payload_type { OpenSocialNet::Network::PayloadType::H264 }; // kind stamped on every packet

    };

}

#endif // VIDEO_PACKETIZER_HH
