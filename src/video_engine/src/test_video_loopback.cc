// related headers
#include "VideoCapture.hh"
#include "VideoEncoder.hh"
#include "VideoDecoder.hh"
#include "VideoRenderer.hh"
#include "VideoPacketizer.hh"
#include "VideoReassembler.hh"

// c sys headers
#include <cstddef>
#include <cstdint>
#include <cstdio>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "NetworkConstants.hh"
#include "Packet.hh"
#include "PacketJitterBuffer.hh"
#include "UdpReceiver.hh"
#include "UdpSender.hh"

static std::atomic<bool> running { true };

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

int main()
{

    constexpr std::uint16_t video_port { 9001 };

    ::printf("Video loopback test (port %u)\n", static_cast<unsigned>(video_port));
    ::printf("===============================\n\n");

    if (!::SDL_Init(SDL_INIT_VIDEO))
    {

        ::printf("ERROR: SDL_Init failed: %s\n", ::SDL_GetError());
        return 1;

    }

    int width { 640 };
    int height { 480 };
    int fps { 30 };

    OpenSocialNet::Video::VideoCapture capture { };
    OpenSocialNet::Video::VideoEncoder encoder { };
    OpenSocialNet::Video::VideoDecoder decoder { };
    OpenSocialNet::Video::VideoRenderer renderer { };
    OpenSocialNet::Video::VideoPacketizer packetizer { };
    OpenSocialNet::Video::VideoReassembler reassembler { };

    OpenSocialNet::Network::UdpSender sender { };
    OpenSocialNet::Network::UdpReceiver receiver { };
    OpenSocialNet::Network::PacketJitterBuffer jitter_buffer { };

    if (!capture.init("/dev/video0", width, height, fps)) { ::printf("capture init failed (is /dev/video0 attached and readable?)\n"); return 1; }
    if (!encoder.init(width, height, fps)) { ::printf("encoder init failed\n"); return 1; }
    if (!decoder.init()) { ::printf("decoder init failed\n"); return 1; }
    if (!renderer.init(width, height, "Video Loopback")) { ::printf("renderer init failed: %s\n", ::SDL_GetError()); return 1; }
    if (!receiver.init(video_port)) { ::printf("receiver init failed\n"); return 1; }
    if (!sender.init(OpenSocialNet::Network::ipv4_loopback_address, video_port)) { ::printf("sender init failed\n"); return 1; }

    // pick a random ssrc for this stream; 90kHz / 30fps = 3000 ticks per frame (RTP convention)
    std::mt19937 rng { std::random_device { }() };
    std::uint32_t ssrc { std::uniform_int_distribution<std::uint32_t> { }(rng) };
    packetizer.init(ssrc, 3000);

    std::thread rx { receive_thread, std::ref(receiver), std::ref(jitter_buffer) };

    // allocate input YUV and intermediate H.264 buffers
    std::size_t yuv_size { static_cast<std::size_t>(width * height) * 3 / 2 };
    std::vector<std::uint8_t> yuv_frame(yuv_size);
    std::vector<std::byte> h264_buf(yuv_size);

    int frames_captured { 0 };
    int frames_sent { 0 };
    int packets_sent { 0 };
    int frames_rendered { 0 };

    ::printf("Sender + receiver running. Close window to exit.\n\n");

    while (running)
    {

        ::SDL_Event ev { };
        while (::SDL_PollEvent(&ev))
        {

            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;

        }

        using clock = std::chrono::steady_clock;
        auto t0 { clock::now() };

        // capture next camera frame, encode it, packetize, and send each packet
        std::size_t captured_bytes { capture.capture_frame({ yuv_frame.data(), yuv_size }) };
        if (captured_bytes == 0) continue;
        ++frames_captured;
        auto t_cap { clock::now() };

        int h264_bytes { encoder.encode_frame(yuv_frame.data(), width, { h264_buf.data(), h264_buf.size() }) };
        auto t_enc { clock::now() };
        if (h264_bytes > 0)
        {

            auto packets { packetizer.packetize_frame({ h264_buf.data(), static_cast<std::size_t>(h264_bytes) }) };
            for (auto& p : packets)
            {

                if (sender.send_raw(p)) ++packets_sent;

            }
            ++frames_sent;

        }
        auto t_send { clock::now() };

        // drain everything currently available in the jitter buffer
        OpenSocialNet::Network::Packet popped { };
        while (jitter_buffer.pop(popped))
        {

            if (!reassembler.feed(popped)) continue;

            auto frame_bytes { reassembler.take_complete_frame() };

            std::uint8_t* decoded_planes[3] { };
            int decoded_strides[3] { };
            if (!decoder.decode_packet(frame_bytes, decoded_planes, decoded_strides)) continue;

            const std::uint8_t* render_planes[3] { decoded_planes[0], decoded_planes[1], decoded_planes[2] };
            if (!renderer.render_frame(render_planes, decoded_strides, width, height))
            {

                ::printf("render_frame failed: %s\n", ::SDL_GetError());
                running = false;
                break;

            }

            ++frames_rendered;
            if (frames_rendered % 30 == 0) ::printf("  rendered %d frames over loopback\n", frames_rendered);

        }
        auto t_rx { clock::now() };

        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        if (frames_captured % 30 == 1) ::printf("  timings(ms): cap=%.1f enc=%.1f send=%.1f rx=%.1f total=%.1f\n", ms(t0, t_cap), ms(t_cap, t_enc), ms(t_enc, t_send), ms(t_send, t_rx), ms(t0, t_rx));

    }

    ::printf("\ncaptured: %d frames, sent: %d frames / %d packets, rendered: %d frames\n", frames_captured, frames_sent, packets_sent, frames_rendered);

    running = false;
    receiver.shutdown();
    rx.join();

    renderer.shutdown();
    ::SDL_Quit();

    return 0;

}
