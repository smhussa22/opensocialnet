#include "VideoRenderer.hh"

// c sys headers
#include <cstring>

namespace OpenSocialNet::Video
{
    VideoRenderer::VideoRenderer() noexcept
        : window(nullptr), renderer(nullptr), texture(nullptr), width_(0), height_(0)
    {
    }

    bool VideoRenderer::init(int width, int height, const std::string& window_title) noexcept
    {
        // Shutdown any existing resources first
        shutdown();

        // Validate input dimensions
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        // Create SDL window
        window = SDL_CreateWindow(
            window_title.c_str(),
            width,
            height,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
        );

        if (!window)
        {
            shutdown();
            return false;
        }

        // Create SDL renderer from window
        renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer)
        {
            shutdown();
            return false;
        }

        // Create SDL texture for YUV420P rendering
        // SDL_PIXELFORMAT_IYUV is the standard planar YUV format
        texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            width,
            height
        );

        if (!texture)
        {
            shutdown();
            return false;
        }

        // Store dimensions
        width_ = width;
        height_ = height;

        return true;
    }

    bool VideoRenderer::render_frame(const uint8_t* yuv420p_planes[3], const int strides[3], int width, int height) noexcept
    {
        if (!valid())
        {
            return false;
        }

        // Check if dimensions changed - recreate texture if needed
        if (width != width_ || height != height_)
        {
            if (!init(width, height, "Video"))
            {
                return false;
            }
        }

        // Validate input parameters
        if (!yuv420p_planes || !yuv420p_planes[0] || !yuv420p_planes[1] || !yuv420p_planes[2])
        {
            return false;
        }

        if (!strides)
        {
            return false;
        }

        // Lock texture for writing
        void* texture_pixels = nullptr;
        int texture_pitch = 0;

        if (!SDL_LockTexture(texture, nullptr, &texture_pixels, &texture_pitch))
        {
            return false;
        }

        uint8_t* dest = static_cast<uint8_t*>(texture_pixels);

        // Calculate plane sizes for YUV420P
        // Y plane: width * height
        // U/V planes: (width/2) * (height/2) each
        int y_plane_size = width_ * height_;
        int uv_plane_size = (width_ / 2) * (height_ / 2);

        // Copy Y plane
        const uint8_t* src_y = yuv420p_planes[0];
        for (int i = 0; i < height_; ++i)
        {
            std::memcpy(dest, src_y, width_);
            dest += texture_pitch;
            src_y += strides[0];
        }

        // Copy U plane (interleaved in the middle section)
        const uint8_t* src_u = yuv420p_planes[1];
        for (int i = 0; i < height_ / 2; ++i)
        {
            std::memcpy(dest, src_u, width_ / 2);
            dest += texture_pitch / 2;
            src_u += strides[1];
        }

        // Copy V plane
        const uint8_t* src_v = yuv420p_planes[2];
        for (int i = 0; i < height_ / 2; ++i)
        {
            std::memcpy(dest, src_v, width_ / 2);
            dest += texture_pitch / 2;
            src_v += strides[2];
        }

        // Unlock texture
        SDL_UnlockTexture(texture);

        // Render texture to screen
        if (!SDL_RenderTexture(renderer, texture, nullptr, nullptr))
        {
            return false;
        }

        // Present the renderer
        if (!SDL_RenderPresent(renderer))
        {
            return false;
        }

        return true;
    }

    void VideoRenderer::shutdown() noexcept
    {
        // Destroy in reverse order of creation
        if (texture)
        {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }

        if (renderer)
        {
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }

        if (window)
        {
            SDL_DestroyWindow(window);
            window = nullptr;
        }

        width_ = 0;
        height_ = 0;
    }

    bool VideoRenderer::valid() const noexcept
    {
        return window != nullptr;
    }

}
