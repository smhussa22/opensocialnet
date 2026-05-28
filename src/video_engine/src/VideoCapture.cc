#include "VideoCapture.hh"

namespace OpenSocialNet::Video
{
    VideoCapture::VideoCapture() noexcept {
    }

    bool VideoCapture::init(const char* device_path, int width, int height, int framerate) noexcept {
    }

    void VideoCapture::shutdown() noexcept {
    }

    size_t VideoCapture::capture_frame(std::span<uint8_t> frame_buffer) noexcept {
    }

    bool VideoCapture::valid() const noexcept {
    }

    int VideoCapture::width() const noexcept {
    }

    int VideoCapture::height() const noexcept {
    }

}
