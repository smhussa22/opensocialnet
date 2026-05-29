// related headers
#include "VideoCapture.hh"
#include "VideoEncoder.hh"
#include "VideoPacketizer.hh"

// c sys headers
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// cpp stdlib headers
#include <atomic>
#include <csignal>
#include <random>
#include <string_view>
#include <vector>

// 3rd party headers

// project headers
#include "NetworkConstants.hh"
#include "UdpSender.hh"

static std::atomic<bool> running { true };

void on_signal(int) { running = false; }

int main(int argc, char** argv)
{

    // parse host and port from argv, with loopback + 9001 defaults
    std::string_view host { argc > 1 ? argv[1] : OpenSocialNet::Network::ipv4_loopback_address };
    std::uint16_t port { argc > 2 ? static_cast<std::uint16_t>(std::atoi(argv[2])) : static_cast<std::uint16_t>(9001) };

    int width { 640 };
    int height { 480 };
    int fps { 30 };

    ::printf("video_sender: streaming /dev/video0 -> %.*s:%u (%dx%d @ %d fps)\n", static_cast<int>(host.size()), host.data(), static_cast<unsigned>(port), width, height, fps);

    OpenSocialNet::Video::VideoCapture capture { };
    OpenSocialNet::Video::VideoEncoder encoder { };
    OpenSocialNet::Video::VideoPacketizer packetizer { };
    OpenSocialNet::Network::UdpSender sender { };

    if (!capture.init("/dev/video0", width, height, fps)) { ::printf("capture init failed (is /dev/video0 attached and readable?)\n"); return 1; }
    if (!encoder.init(width, height, fps)) { ::printf("encoder init failed\n"); return 1; }
    if (!sender.init(host, port)) { ::printf("sender init failed\n"); return 1; }

    // pick a random ssrc; 90kHz / fps = 3000 ticks/frame at 30 fps (RTP convention)
    std::mt19937 rng { std::random_device { }() };
    std::uint32_t ssrc { std::uniform_int_distribution<std::uint32_t> { }(rng) };
    packetizer.init(ssrc, 90000 / fps);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // allocate input YUV and intermediate H.264 buffers
    std::size_t yuv_size { static_cast<std::size_t>(width * height) * 3 / 2 };
    std::vector<std::uint8_t> yuv_frame(yuv_size);
    std::vector<std::byte> h264_buf(yuv_size);

    int frames_captured { 0 };
    int frames_sent { 0 };
    int packets_sent { 0 };

    ::printf("running. Ctrl-C to stop.\n");

    while (running)
    {

        std::size_t captured_bytes { capture.capture_frame({ yuv_frame.data(), yuv_size }) };
        if (captured_bytes == 0) continue;
        ++frames_captured;

        int h264_bytes { encoder.encode_frame(yuv_frame.data(), width, { h264_buf.data(), h264_buf.size() }) };
        if (h264_bytes <= 0) continue;

        auto packets { packetizer.packetize_frame({ h264_buf.data(), static_cast<std::size_t>(h264_bytes) }) };
        for (auto& p : packets)
        {

            if (sender.send_raw(p)) ++packets_sent;

        }
        ++frames_sent;

        if (frames_sent % 30 == 0) ::printf("  sent %d frames / %d packets\n", frames_sent, packets_sent);

    }

    ::printf("\ncaptured: %d, encoded+sent: %d frames / %d packets\n", frames_captured, frames_sent, packets_sent);

    return 0;

}
