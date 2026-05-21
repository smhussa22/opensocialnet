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
#include "AudioEncoder.hh"
#include "AudioDecoder.hh"
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

struct PlaybackContext
{

    OpenSocialNet::Network::PacketJitterBuffer* jitter_buffer {};
    OpenSocialNet::Audio::AudioDecoder*         decoder {};

};

static void on_playback(void* userdata, SDL_AudioStream* stream, int additional, int total)
{

    auto* context = static_cast<PlaybackContext*>(userdata);
    OpenSocialNet::Network::Packet packet {};
    std::array<float, OpenSocialNet::Audio::samples_per_frame> decoded_pcm {};
    constexpr int decoded_byte_count { static_cast<int>(decoded_pcm.size() * sizeof(float)) };

    int fed = 0;
    while (fed < additional)
    {
        if (!context->jitter_buffer->pop(packet)) break;

        const int decoded_samples { context->decoder->decode_bytes(
            std::span<const std::byte> { reinterpret_cast<const std::byte*>(packet.payload), packet.header.payload_size },
            std::span<float>           { decoded_pcm }
        ) };
        if (decoded_samples < 0) continue; // skip corrupted frames; keep the stream alive

        SDL_PutAudioStreamData(stream, decoded_pcm.data(), decoded_byte_count);
        fed += decoded_byte_count;
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

        OpenSocialNet::Audio::AudioDecoder decoder {};
        if (!decoder.valid())
        {
            std::cout << "[main] Failed to init Opus decoder\n";
            return 1;
        }
        std::cout << "[main] decoder init ok\n";

        OpenSocialNet::Audio::AudioEncoder encoder {};
        if (!encoder.valid())
        {
            std::cout << "[main] Failed to init Opus encoder\n";
            return 1;
        }
        std::cout << "[main] encoder init ok\n";

        PlaybackContext playback_context { &jitter_buffer, &decoder };
        SDL_AudioSpec spec { OpenSocialNet::Audio::create_opus_audio_spec() };
        OpenSocialNet::Audio::AudioStream audio_stream { spec, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, on_playback, &playback_context };
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

            while (capture.available() >= OpenSocialNet::Audio::samples_per_frame)
            {

                size_t got = capture.read(std::span<float>{chunk});
                if (got != OpenSocialNet::Audio::samples_per_frame) break;

                OpenSocialNet::Network::Packet packet {};
                const int encoded_bytes { encoder.encode_samples(
                    std::span<const float>     { chunk.data(), got },
                    std::span<std::byte>       { reinterpret_cast<std::byte*>(packet.payload), OpenSocialNet::Network::maximum_packet_size }
                ) };
                if (encoded_bytes <= 0) continue;

                packet.header.payload_type = OpenSocialNet::Network::PayloadType::Opus;
                packet.header.payload_size = static_cast<uint16_t>(encoded_bytes);

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