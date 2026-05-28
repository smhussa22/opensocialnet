#include "VideoRenderer.hh"

namespace OpenSocialNet::Video
{
    VideoRenderer::VideoRenderer() noexcept {
    }

    bool VideoRenderer::init(int width, int height, const std::string& window_title) noexcept {
    }

    bool VideoRenderer::render_frame(const uint8_t* yuv420p_planes[3], const int strides[3], int width, int height) noexcept {
    }

    void VideoRenderer::shutdown() noexcept {
    }

    bool VideoRenderer::valid() const noexcept {
    }

}
