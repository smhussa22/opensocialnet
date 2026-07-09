#ifndef VIDEO_DECODER_HH
#define VIDEO_DECODER_HH

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <span>

// 3rd party headers
extern "C"
{

#include <libavcodec/avcodec.h>

}

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
        // packets received before the first SPS/IDR are dropped silently so a
        // mid-stream join doesn't flood the decoder with unreferencable slices.
        bool decode_packet(std::span<const std::byte> h264_data, std::uint8_t** yuv420p_planes, int* strides) noexcept;

        // flushes remaining frames from decoder. returns true if frame available.
        bool flush(std::uint8_t** yuv420p_planes, int* strides) noexcept;

        // resets decoder state.
        void reset() noexcept;

        // returns whether decoder was successfully initialized.
        [[nodiscard]] bool valid() const noexcept;

        // returns frame width/height (0 if not yet decoded).
        [[nodiscard]] int width() const noexcept;
        [[nodiscard]] int height() const noexcept;

    private:
        AVCodecContextPtr codec_ctx { }; // libavcodec context
        AVPacketPtr packet { }; // incoming packet buffer
        AVFramePtr frame { }; // decoded frame buffer
        int res_width { 0 }; // decoded frame width
        int res_height { 0 }; // decoded frame height
        bool synced { false }; // true once the first SPS/IDR has been seen

    };

}

#endif // VIDEO_DECODER_HH
