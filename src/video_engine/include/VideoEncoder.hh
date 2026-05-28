#ifndef VIDEO_ENCODER_HH
#define VIDEO_ENCODER_HH

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <span>

// 3rd party headers
#include <x264.h>

// project headers
#include "VideoEncoderDeleter.hh"

namespace OpenSocialNet::Video
{
    class VideoEncoder
    {
    public:
        explicit VideoEncoder() noexcept;
        ~VideoEncoder() = default;

        VideoEncoder(const VideoEncoder&) = delete;
        VideoEncoder& operator=(const VideoEncoder&) = delete;
        VideoEncoder(VideoEncoder&&) = default;
        VideoEncoder& operator=(VideoEncoder&&) = default;

        // initializes encoder with resolution and framerate. returns true on success.
        bool init(int width, int height, int framerate) noexcept;

        // encodes a raw YUV420P frame into H.264 bitstream. returns number of bytes written.
        int encode_frame(const uint8_t* yuv420p_data, int stride, std::span<std::byte> output_h264_bytes) noexcept;

        // flushes remaining frames from encoder. returns number of bytes in final packet.
        int flush(std::span<std::byte> output_h264_bytes) noexcept;

        // resets encoder state.
        void reset() noexcept;

        // returns whether encoder was successfully initialized.
        [[nodiscard]] bool valid() const noexcept;

    private:
        X264EncoderPtr encoder {};
        x264_picture_t pic_in {};
        int width_ {0};
        int height_ {0};
    };

}

#endif // VIDEO_ENCODER_HH