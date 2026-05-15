// related headers

// c sys headers
#include <csignal>
#include <cstring>

// cpp stdlib headers
#include <thread>
#include <atomic>
#include <array>
#include <iostream>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "NetworkConstants.hh"
#include "Packet.hh"
#include "UdpSender.hh"
#include "UdpReceiver.hh"
#include "AudioStream.hh"
#include "AudioCapture.hh"
#include "AudioConstants.hh"
#include "PacketJitterBuffer.hh"

static std::atomic<bool> running {true};

static void on_signal(int)
{
    running = false;
}

void receive_thread(OpenSocialNet::Network::UdpReceiver& receiver, OpenSocialNet::Network::PacketJitterBuffer& jitter_buffer)
{

    OpenSocialNet::Network::Packet packet {};

    while (running)
    {

        if (!receiver.receive(packet)) continue;
        if (packet.header.payload_size == 0) continue;
        if (packet.header.payload_size > OpenSocialNet::Network::maximum_packet_size) continue;

        std::cout << "payload_size=" << packet.header.payload_size << " expected=1920\n";
        
        jitter_buffer.push(packet);

    }

}

int main()
{
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!SDL_Init(SDL_INIT_AUDIO))
    {
        std::cout << "SDL init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    std::cout << "[main] SDL init ok\n";

    SDL_AudioSpec spec { OpenSocialNet::Audio::create_opus_audio_spec() };
    OpenSocialNet::Audio::AudioStream audio_stream { spec };
    audio_stream.resume();
    std::cout << "[main] playback stream open\n";

    OpenSocialNet::Audio::AudioCapture capture {};
    if (!capture.init())
    {
        std::cout << "[main] Failed to init capture\n";
        return 1;
    }
    std::cout << "[main] capture init ok\n";

    OpenSocialNet::Network::UdpReceiver receiver {};
    if (!receiver.init())
    {
        std::cout << "[main] Failed to init receiver\n";
        return 1;
    }
    std::cout << "[main] receiver init ok\n";

    OpenSocialNet::Network::UdpSender sender {};
    if (!sender.init())
    {
        std::cout << "[main] Failed to init sender\n";
        return 1;
    }
    std::cout << "[main] sender init ok\n";

    OpenSocialNet::Network::PacketJitterBuffer jitter_buffer {};
    OpenSocialNet::Network::Packet out_packet {};

    std::thread rx { receive_thread, std::ref(receiver), std::ref(jitter_buffer) };

    std::array<float, 480> chunk {};
    std::cout << "[main] starting capture loop...\n";

    int packets_sent = 0;

    while (running)
    {

        size_t avail = capture.available();

        if (avail >= 480)
        {
            size_t got = capture.read(std::span<float>{chunk});
            if (got == 0)
            {
                std::cout << "[main] read returned 0 despite available=" << avail << "\n";
                continue;
            }

            OpenSocialNet::Network::Packet packet {};
            packet.header.payload_type = OpenSocialNet::Network::PayloadType::PCM;
            packet.header.payload_size = static_cast<uint16_t>(got * sizeof(float));
            std::memcpy(packet.payload, chunk.data(), packet.header.payload_size);

            bool ok = sender.send(packet);
            ++packets_sent;

            if (packets_sent % 50 == 0)
                std::cout << "[main] sent " << packets_sent << " packets, last size="
                          << packet.header.payload_size << " send_ok=" << ok << "\n";
        }
        else if (packets_sent == 0)
        {
            // print every 500ms if we never get any samples
            static int ticks = 0;
            if (++ticks % 50 == 0)
                std::cout << "[main] waiting for mic samples, available=" << avail << "\n";
        }
        
        if (jitter_buffer.pop(out_packet))
        {

            audio_stream.put_audio_data(out_packet.payload, out_packet.header.payload_size);

        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "[main] shutting down, total packets sent=" << packets_sent << "\n";

    running = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    receiver.shutdown();
    rx.join();

    capture.shutdown();
    SDL_Quit();
    return 0;

}