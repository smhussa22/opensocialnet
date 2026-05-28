#ifndef VIDEO_RENDERER_HH
#define VIDEO_RENDERER_HH

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <memory>
#include <string>

// 3rd party headers
#include <SDL3/SDL.h>

namespace OpenSocialNet::Video
{
    class VideoRenderer
    {
    public:
        explicit VideoRenderer() noexcept;
        ~VideoRenderer() { shutdown(); };

        VideoRenderer(const VideoRenderer&) = delete;
        VideoRenderer& operator=(const VideoRenderer&) = delete;

        VideoRenderer(VideoRenderer&&) = delete;
        VideoRenderer& operator=(VideoRenderer&&) = delete;
 
        // creates SDL window and renderer for video display. returns true on success.
        bool init(int width, int height, const std::string& window_title = "Video") noexcept;

        // displays a YUV420P frame. returns true on success.
        bool render_frame(const uint8_t* yuv420p_planes[3], const int strides[3], int width, int height) noexcept;

        // closes SDL window and renderer.
        void shutdown() noexcept;

        // returns whether renderer was successfully initialized.
        [[nodiscard]] bool valid() const noexcept;

    private:
        SDL_Window* window {};
        SDL_Renderer* renderer {};
        SDL_Texture* texture {};
        int width_ {0};
        int height_ {0};
    
    };

}

#endif // VIDEO_RENDERER_HH