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
        
        jitter_buffer.push(packet);

    }

}

static void on_playback(void* userdata, SDL_AudioStream* stream, int additional, int total)
{

    auto* jitter = static_cast<OpenSocialNet::Network::PacketJitterBuffer*>(userdata);
    OpenSocialNet::Network::Packet packet {};
    int fed = 0;
    while (fed < additional)
    {
        if (!jitter->pop(packet)) break;
        SDL_PutAudioStreamData(stream, packet.payload, packet.header.payload_size);
        fed += packet.header.payload_size;
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

    {

        OpenSocialNet::Network::PacketJitterBuffer jitter_buffer {};
        SDL_AudioSpec spec { OpenSocialNet::Audio::create_opus_audio_spec() };
        OpenSocialNet::Audio::AudioStream audio_stream { spec, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, on_playback, &jitter_buffer };
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

        std::thread rx { receive_thread, std::ref(receiver), std::ref(jitter_buffer) };

        std::array<float, 480> chunk {};
        std::cout << "[main] starting capture loop...\n";

        int packets_sent = 0;

        while (running)
        {

            while (capture.available() >= 480)
            {

                size_t got = capture.read(std::span<float>{chunk});
                if (got == 0) break;
                OpenSocialNet::Network::Packet packet {};
                packet.header.payload_type = OpenSocialNet::Network::PayloadType::PCM;
                packet.header.payload_size = static_cast<uint16_t>(got * sizeof(float));
                std::memcpy(packet.payload, chunk.data(), packet.header.payload_size);

                sender.send(packet);
                ++packets_sent;

            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        }

        std::cout << "[main] shutting down, total packets sent=" << packets_sent << "\n";

        running = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        receiver.shutdown();
        rx.join();

        capture.shutdown();

    } // audio_stream + jitter_buffer + sender + receiver destroyed here, before SDL_Quit

    SDL_Quit();
    return 0;

}