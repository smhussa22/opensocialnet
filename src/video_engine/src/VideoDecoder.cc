#include "VideoDecoder.hh"

namespace OpenSocialNet::Video
{
    VideoDecoder::VideoDecoder() noexcept {
    }

    bool VideoDecoder::init() noexcept {
    }

    bool VideoDecoder::decode_packet(std::span<const std::byte> h264_data, uint8_t** yuv420p_planes, int* strides) noexcept {
    }

    bool VideoDecoder::flush(uint8_t** yuv420p_planes, int* strides) noexcept {
    }

    void VideoDecoder::reset() noexcept {
    }

    bool VideoDecoder::valid() const noexcept {
    }

    int VideoDecoder::width() const noexcept {
    }

    int VideoDecoder::height() const noexcept {
    }

}
