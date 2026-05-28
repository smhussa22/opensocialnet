#ifndef VIDEO_DECODER_HH
#define VIDEO_DECODER_HH

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <span>

// 3rd party headers
#include <libavcodec/avcodec.h>

// project headers
#include "VideoDecoderDeleter.hh"

namespace OpenSocialNet::Video
{
    class VideoDecoder
    {
    public:
        VideoDecoder() noexcept = default;
        ~VideoDecoder() = default;

        VideoDecoder(const VideoDecoder&) = delete;
        VideoDecoder& operator=(const VideoDecoder&) = delete;
        VideoDecoder(VideoDecoder&&) = default;
        VideoDecoder& operator=(VideoDecoder&&) = default;

        // initializes decoder for H.264 video. returns true on success.
        bool init() noexcept;

        // decodes H.264 bitstream packet into raw YUV420P frame. returns true if frame ready.
        bool decode_packet(std::span<const std::byte> h264_data, uint8_t** yuv420p_planes, int* strides) noexcept;

        // flushes remaining frames from decoder. returns true if frame available.
        bool flush(uint8_t** yuv420p_planes, int* strides) noexcept;

        // resets decoder state.
        void reset() noexcept;

        // returns whether decoder was successfully initialized.
        [[nodiscard]] bool valid() const noexcept;

        // returns frame width/height (0 if not yet decoded).
        [[nodiscard]] int width() const noexcept;
        [[nodiscard]] int height() const noexcept;

    private:
        AVCodecContextPtr codec_ctx { };
        AVPacketPtr packet { };
        AVFramePtr frame { };
        int res_width { 0 };
        int res_height { 0 };
    };

}

#endif // VIDEO_DECODER_HH
