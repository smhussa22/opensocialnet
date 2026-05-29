#ifndef VIDEO_REASSEMBLER_HH
#define VIDEO_REASSEMBLER_HH

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

    // Accumulates packet payloads into complete H.264 frames.
    // Caller feeds packets one at a time (already in sequence order via jitter buffer).
    // When marker=1 is seen, the assembled frame is available via take_complete_frame().
    class VideoReassembler
    {

    public:
        VideoReassembler() noexcept = default;
        ~VideoReassembler() = default;

        VideoReassembler(const VideoReassembler&) = delete;
        VideoReassembler& operator=(const VideoReassembler&) = delete;
        VideoReassembler(VideoReassembler&&) = default;
        VideoReassembler& operator=(VideoReassembler&&) = default;

        // Appends packet payload. Returns true if marker=1 packet just completed a frame.
        // A timestamp change without marker=1 drops the partial frame (lost fragment).
        bool feed(const OpenSocialNet::Network::Packet& packet) noexcept;

        // Returns the assembled frame bytes; valid until next feed() that starts a new frame.
        std::span<const std::byte> take_complete_frame() const noexcept;

    private:
        std::vector<std::byte> frame_buffer { };  // accumulating bytes for the in-progress frame
        std::uint32_t current_timestamp { 0 };    // timestamp of the in-progress frame, 0 = none yet
        bool have_timestamp { false };            // whether current_timestamp is valid
        bool frame_ready { false };               // true after marker=1; cleared on next feed()

    };

}

#endif // VIDEO_REASSEMBLER_HH
