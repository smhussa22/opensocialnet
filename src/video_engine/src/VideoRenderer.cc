#include "VideoRenderer.hh"

// c sys headers
#include <cstring>

namespace OpenSocialNet::Video
{

    VideoRenderer::VideoRenderer() noexcept
    {
    }

    bool VideoRenderer::init(int width, int height, const std::string& window_title) noexcept
    {

        shutdown();

        if (width <= 0 || height <= 0) return false;

        // create SDL window and renderer
        window = ::SDL_CreateWindow(window_title.c_str(), width, height, SDL_WINDOW_RESIZABLE);
        if (!window)
        {

            shutdown();
            return false;

        }

        renderer = ::SDL_CreateRenderer(window, nullptr);
        if (!renderer)
        {

            shutdown();
            return false;

        }

        // create YUV420P streaming texture
        texture = ::SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!texture)
        {

            shutdown();
            return false;

        }

        width_ = width;
        height_ = height;

        return true;

    }

    bool VideoRenderer::render_frame(const std::uint8_t* yuv420p_planes[3], const int strides[3], int width, int height) noexcept
    {

        if (!valid()) return false;

        // recreate texture if dimensions changed
        if (width != width_ || height != height_)
        {

            if (!init(width, height, "Video")) return false;

        }

        if (!yuv420p_planes || !yuv420p_planes[0] || !yuv420p_planes[1] || !yuv420p_planes[2]) return false;
        if (!strides) return false;

        // lock texture, blit Y/U/V planes, unlock and present
        void* texture_pixels { nullptr };
        int texture_pitch { 0 };

        if (!::SDL_LockTexture(texture, nullptr, &texture_pixels, &texture_pitch)) return false;

        std::uint8_t* dest { static_cast<std::uint8_t*>(texture_pixels) };

        const std::uint8_t* src_y { yuv420p_planes[0] };
        for (int i { 0 }; i < height_; ++i)
        {

            std::memcpy(dest, src_y, width_);
            dest += texture_pitch;
            src_y += strides[0];

        }

        const std::uint8_t* src_u { yuv420p_planes[1] };
        for (int i { 0 }; i < height_ / 2; ++i)
        {

            std::memcpy(dest, src_u, width_ / 2);
            dest += texture_pitch / 2;
            src_u += strides[1];

        }

        const std::uint8_t* src_v { yuv420p_planes[2] };
        for (int i { 0 }; i < height_ / 2; ++i)
        {

            std::memcpy(dest, src_v, width_ / 2);
            dest += texture_pitch / 2;
            src_v += strides[2];

        }

        ::SDL_UnlockTexture(texture);

        if (!::SDL_RenderTexture(renderer, texture, nullptr, nullptr)) return false;
        if (!::SDL_RenderPresent(renderer)) return false;

        return true;

    }

    void VideoRenderer::shutdown() noexcept
    {

        // destroy SDL resources in reverse creation order
        if (texture)
        {

            ::SDL_DestroyTexture(texture);
            texture = nullptr;

        }

        if (renderer)
        {

            ::SDL_DestroyRenderer(renderer);
            renderer = nullptr;

        }

        if (window)
        {

            ::SDL_DestroyWindow(window);
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
