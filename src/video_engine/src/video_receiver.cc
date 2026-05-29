// related headers
#include "VideoDecoder.hh"
#include "VideoRenderer.hh"
#include "VideoReassembler.hh"

// c sys headers
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// cpp stdlib headers
#include <atomic>
#include <csignal>
#include <thread>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "NetworkConstants.hh"
#include "Packet.hh"
#include "PacketJitterBuffer.hh"
#include "UdpReceiver.hh"

static std::atomic<bool> running { true };

void on_signal(int) { running = false; }

void receive_thread(OpenSocialNet::Network::UdpReceiver& receiver, OpenSocialNet::Network::PacketJitterBuffer& jitter_buffer)
{

    OpenSocialNet::Network::Packet packet { };

    while (running)
    {

        if (!receiver.receive(packet)) continue;
        if (packet.header.payload_size == 0) continue;
        if (packet.header.payload_size > OpenSocialNet::Network::maximum_packet_size) continue;

        jitter_buffer.push(packet);

    }

}

int main(int argc, char** argv)
{

    std::uint16_t port { argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : static_cast<std::uint16_t>(9001) };

    int width { 640 };
    int height { 480 };

    ::printf("video_receiver: listening on port %u, decoding to %dx%d\n", static_cast<unsigned>(port), width, height);

    if (!::SDL_Init(SDL_INIT_VIDEO))
    {

        ::printf("ERROR: SDL_Init failed: %s\n", ::SDL_GetError());
        return 1;

    }

    OpenSocialNet::Video::VideoDecoder decoder { };
    OpenSocialNet::Video::VideoRenderer renderer { };
    OpenSocialNet::Video::VideoReassembler reassembler { };
    OpenSocialNet::Network::UdpReceiver receiver { };
    OpenSocialNet::Network::PacketJitterBuffer jitter_buffer { };

    if (!decoder.init()) { ::printf("decoder init failed\n"); return 1; }
    if (!renderer.init(width, height, "Video Receiver")) { ::printf("renderer init failed: %s\n", ::SDL_GetError()); return 1; }
    if (!receiver.init(port)) { ::printf("receiver init failed\n"); return 1; }

    std::thread rx { receive_thread, std::ref(receiver), std::ref(jitter_buffer) };

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    int frames_decoded { 0 };
    int frames_rendered { 0 };

    ::printf("waiting for video packets... close window to exit, or Ctrl-C\n\n");

    while (running)
    {

        ::SDL_Event ev { };
        while (::SDL_PollEvent(&ev))
        {

            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;

        }

        // drain everything currently available in the jitter buffer
        OpenSocialNet::Network::Packet popped { };
        while (jitter_buffer.pop(popped))
        {

            if (!reassembler.feed(popped)) continue;

            auto frame_bytes { reassembler.take_complete_frame() };

            std::uint8_t* decoded_planes[3] { };
            int decoded_strides[3] { };
            if (!decoder.decode_packet(frame_bytes, decoded_planes, decoded_strides)) continue;
            ++frames_decoded;

            const std::uint8_t* render_planes[3] { decoded_planes[0], decoded_planes[1], decoded_planes[2] };
            if (!renderer.render_frame(render_planes, decoded_strides, width, height))
            {

                ::printf("render_frame failed: %s\n", ::SDL_GetError());
                running = false;
                break;

            }

            ++frames_rendered;
            if (frames_rendered % 30 == 0) ::printf("  rendered %d frames\n", frames_rendered);

        }

    }

    ::printf("\ndecoded: %d frames, rendered: %d frames\n", frames_decoded, frames_rendered);

    running = false;
    receiver.shutdown();
    rx.join();

    renderer.shutdown();
    ::SDL_Quit();

    return 0;

}
