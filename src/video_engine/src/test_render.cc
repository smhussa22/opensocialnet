// related headers
#include "VideoRenderer.hh"

// c sys headers
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <cstring>

// cpp stdlib headers
#include <vector>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers

void generate_test_frame(std::uint8_t* yuv420p, int width, int height, int frame_num)
{

    std::size_t y_size { static_cast<std::size_t>(width * height) };
    std::size_t uv_size { static_cast<std::size_t>((width / 2) * (height / 2)) };

    std::uint8_t* y_plane { yuv420p };
    std::uint8_t* u_plane { yuv420p + y_size };
    std::uint8_t* v_plane { yuv420p + y_size + uv_size };

    // moving luma pattern, static chroma
    for (std::size_t i { 0 }; i < y_size; ++i)
    {

        y_plane[i] = static_cast<std::uint8_t>((i + frame_num * 10) % 256);

    }

    std::memset(u_plane, 128, uv_size);
    std::memset(v_plane, 128, uv_size);

}

int main()
{

    ::printf("Testing VideoRenderer with synthetic frames\n");
    ::printf("============================================\n\n");

    if (!::SDL_Init(SDL_INIT_VIDEO))
    {

        ::printf("ERROR: SDL_Init failed: %s\n", ::SDL_GetError());
        return 1;

    }

    int width { 640 };
    int height { 480 };

    OpenSocialNet::Video::VideoRenderer renderer;

    ::printf("Opening %dx%d window\n", width, height);
    if (!renderer.init(width, height, "VideoRenderer Test"))
    {

        ::printf("ERROR: renderer.init failed: %s\n", ::SDL_GetError());
        ::SDL_Quit();
        return 1;

    }
    ::printf("  Window ready. Close it or press Ctrl+C to exit.\n\n");

    // allocate one reusable YUV420P frame buffer and plane pointers for the renderer
    std::size_t yuv_size { static_cast<std::size_t>(width * height) * 3 / 2 };
    std::vector<std::uint8_t> yuv_frame(yuv_size);

    const std::uint8_t* planes[3] { };
    int strides[3] { };
    planes[0] = yuv_frame.data();
    planes[1] = yuv_frame.data() + width * height;
    planes[2] = yuv_frame.data() + width * height + (width / 2) * (height / 2);
    strides[0] = width;
    strides[1] = width / 2;
    strides[2] = width / 2;

    // animate frames at ~30fps until window is closed
    int frame_num { 0 };
    bool running { true };
    while (running)
    {

        ::SDL_Event ev { };
        while (::SDL_PollEvent(&ev))
        {

            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;

        }

        generate_test_frame(yuv_frame.data(), width, height, frame_num);

        if (!renderer.render_frame(planes, strides, width, height))
        {

            ::printf("ERROR: render_frame failed: %s\n", ::SDL_GetError());
            break;

        }

        ++frame_num;
        ::SDL_Delay(33);

    }

    ::printf("\nRendered %d frames. Exiting.\n", frame_num);

    renderer.shutdown();
    ::SDL_Quit();

    return 0;

}
