// related headers
#include "VideoEncoder.hh"
#include "VideoDecoder.hh"
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

    ::printf("Testing round-trip: YUV -> encode -> decode -> render\n");
    ::printf("======================================================\n\n");

    if (!::SDL_Init(SDL_INIT_VIDEO))
    {

        ::printf("ERROR: SDL_Init failed: %s\n", ::SDL_GetError());
        return 1;

    }

    int width { 640 };
    int height { 480 };
    int fps { 30 };

    OpenSocialNet::Video::VideoEncoder encoder {};
    OpenSocialNet::Video::VideoDecoder decoder {};
    OpenSocialNet::Video::VideoRenderer renderer {};

    if (!encoder.init(width, height, fps))
    {

        ::printf("ERROR: encoder init failed\n");
        ::SDL_Quit();
        return 1;

    }

    if (!decoder.init())
    {

        ::printf("ERROR: decoder init failed\n");
        ::SDL_Quit();
        return 1;

    }

    if (!renderer.init(width, height, "Roundtrip Test"))
    {

        ::printf("ERROR: renderer init failed: %s\n", ::SDL_GetError());
        ::SDL_Quit();
        return 1;

    }

    ::printf("All components ready. Close window to exit.\n\n");

    // allocate input YUV buffer and intermediate H.264 buffer
    std::size_t yuv_size { static_cast<std::size_t>(width * height) * 3 / 2 };
    std::vector<std::uint8_t> yuv_frame(yuv_size);
    std::vector<std::byte> h264_buf(yuv_size);

    int frame_num { 0 };
    int frames_rendered { 0 };
    bool running { true };

    while (running)
    {

        ::SDL_Event ev { };
        while (::SDL_PollEvent(&ev))
        {

            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;

        }

        // generate next synthetic frame and push through encoder
        generate_test_frame(yuv_frame.data(), width, height, frame_num);

        int h264_bytes { encoder.encode_frame(yuv_frame.data(), width, {h264_buf.data(), h264_buf.size()}) };
        ++frame_num;

        // encoder buffers first ~10 frames before emitting output; skip until then
        if (h264_bytes <= 0)
        {

            ::SDL_Delay(33);
            continue;

        }

        // feed H.264 bytes to decoder and grab decoded planes
        std::uint8_t* decoded_planes[3] { };
        int decoded_strides[3] { };
        std::span<const std::byte> h264_span { h264_buf.data(), static_cast<std::size_t>(h264_bytes) };

        if (!decoder.decode_packet(h264_span, decoded_planes, decoded_strides))
        {

            ::SDL_Delay(33);
            continue;

        }

        // render the decoded frame (cast away non-const for the renderer's plane-pointer API)
        const std::uint8_t* render_planes[3] {
            decoded_planes[0], decoded_planes[1], decoded_planes[2]
        };

        if (!renderer.render_frame(render_planes, decoded_strides, width, height))
        {

            ::printf("ERROR: render_frame failed: %s\n", ::SDL_GetError());
            break;

        }

        ++frames_rendered;
        if (frames_rendered % 30 == 0) ::printf("  rendered %d decoded frames\n", frames_rendered);

        ::SDL_Delay(33);

    }

    ::printf("\nPushed %d frames into encoder, rendered %d decoded frames.\n", frame_num, frames_rendered);

    renderer.shutdown();
    ::SDL_Quit();

    return 0;

}
