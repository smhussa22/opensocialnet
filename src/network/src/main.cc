// related headers

// c sys headers
#include <csignal>
#include <cstdio>
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
#include "SpscQueue.hh"

static std::atomic<bool> running {true};

static void on_signal(int)
{
    running = false;
}

// SPSC ring sized for ~1.28s of headroom at 100 packets/s (Opus 10ms frames).
// Power of two so SpscQueue's mask wrap-around fires.
using IncomingQueue = OpenSocialNet::Network::SpscQueue<OpenSocialNet::Network::Packet, 128>;

static std::atomic<std::uint64_t> packets_dropped_overflow { 0 }; // SpscQueue full at try_push

void receive_thread(OpenSocialNet::Network::UdpReceiver& receiver, IncomingQueue& queue)
{

    OpenSocialNet::Network::Packet packet {};

    while (running)
    {

        if (!receiver.receive(packet)) continue;
        if (packet.header.payload_size == 0) continue;
        if (packet.header.payload_size > OpenSocialNet::Network::maximum_packet_size) continue;

        if (!queue.try_push(packet)) packets_dropped_overflow.fetch_add(1, std::memory_order_relaxed);

    }

}

struct PlaybackContext
{

    IncomingQueue*                              incoming {};       // recv thread -> playback hand-off
    OpenSocialNet::Network::PacketJitterBuffer* jitter_buffer {};
    OpenSocialNet::Audio::AudioDecoder*         decoder {};

};

static void on_playback(void* userdata, SDL_AudioStream* stream, int additional, int total)
{

    auto* context = static_cast<PlaybackContext*>(userdata);
    OpenSocialNet::Network::Packet packet {};
    std::array<float, OpenSocialNet::Audio::samples_per_frame> decoded_pcm {};
    constexpr int decoded_byte_count { static_cast<int>(decoded_pcm.size() * sizeof(float)) };

    // drain everything the recv thread parked since the last callback into the
    // jitter buffer; only this thread touches the jitter buffer now, so its
    // internal mutex stays uncontended.
    while (context->incoming->try_pop(packet)) context->jitter_buffer->push(packet);

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

        // ~155 KB on the stack; cheap enough vs. a heap allocation, sized
        // to absorb a burst before the audio callback next drains.
        IncomingQueue incoming_queue {};

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

        PlaybackContext playback_context { &incoming_queue, &jitter_buffer, &decoder };
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

        std::thread rx { receive_thread, std::ref(receiver), std::ref(incoming_queue) };

        std::array<float, 480> chunk {};
        std::cout << "[main] starting capture loop...\n";

        int packets_sent = 0;

        // print a stats line every ~2s; the main loop ticks every 10ms.
        constexpr int stats_tick_interval { 200 };
        int ticks_since_stats { 0 };

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

            if (++ticks_since_stats >= stats_tick_interval)
            {

                const auto snap { jitter_buffer.stats().snapshot() };
                std::printf("[net-stats] obs=%llu lost=%llu ooo=%llu jitter_ms=%.2f spsc=%zu jb=%zu drop_overflow=%llu sent=%d\n",
                    static_cast<unsigned long long>(snap.packets_observed),
                    static_cast<unsigned long long>(snap.packets_lost),
                    static_cast<unsigned long long>(snap.packets_out_of_order),
                    snap.jitter_ms,
                    incoming_queue.size(),
                    jitter_buffer.size(),
                    static_cast<unsigned long long>(packets_dropped_overflow.load(std::memory_order_relaxed)),
                    packets_sent);
                ticks_since_stats = 0;

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