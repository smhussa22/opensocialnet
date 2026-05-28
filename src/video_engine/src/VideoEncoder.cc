#include "VideoEncoder.hh"

namespace OpenSocialNet::Video
{
    VideoEncoder::VideoEncoder() noexcept {
    }

    bool VideoEncoder::init(int width, int height, int framerate) noexcept {
    }

    int VideoEncoder::encode_frame(const uint8_t* yuv420p_data, int stride, std::span<std::byte> output_h264_bytes) noexcept {
    }

    int VideoEncoder::flush(std::span<std::byte> output_h264_bytes) noexcept {
    }

    void VideoEncoder::reset() noexcept {
    }

    bool VideoEncoder::valid() const noexcept {
    }

}
