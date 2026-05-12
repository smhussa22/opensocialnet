#include "NetworkConstants.hh"
#include "Packet.hh"
#include "UdpSender.hh"
#include "UdpReceiver.hh"
#include "AudioStream.hh"
#include "AudioConstants.hh"

#include <SDL3/SDL.h>
#include <cmath>
#include <cstring>
#include <thread>
#include <atomic>
#include <iostream>

using namespace OpenSocialNet::Network;
using namespace OpenSocialNet::Audio;

static std::atomic<bool> running {true};

void receive_thread(UdpReceiver& receiver, AudioStream& audio_stream)
{
    Packet packet {};

    while (running)
    {
        if (!receiver.receive(packet)) continue;

        if (packet.header.payload_size == 0) continue;
        if (packet.header.payload_size > maximum_packet_size) continue;

        std::cout << "Received packet seq=" << packet.header.sequence
                  << " timestamp=" << packet.header.timestamp
                  << " size=" << packet.header.payload_size << "\n";

        audio_stream.put_audio_data(packet.payload, packet.header.payload_size);
    }
}

int main()
{
    if (!SDL_Init(SDL_INIT_AUDIO))
    {
        std::cout << "SDL init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    // init audio
    SDL_AudioSpec spec { create_opus_audio_spec() };
    AudioStream audio_stream { spec };
    audio_stream.resume();

    // init receiver
    UdpReceiver receiver {};
    if (!receiver.init())
    {
        std::cout << "Failed to init receiver\n";
        return 1;
    }

    // init sender
    UdpSender sender {};
    if (!sender.init())
    {
        std::cout << "Failed to init sender\n";
        return 1;
    }

    // start receive thread
    std::thread rx { receive_thread, std::ref(receiver), std::ref(audio_stream) };

    // generate sine wave and send as PCM packets
    // chunk_frames * sizeof(float) must fit within maximum_packet_size (1200)
    const int   chunk_frames = 240;
    const float freq         = 440.0f;
    const float amplitude    = 0.3f;
    const float phase_inc    = 2.0f * static_cast<float>(M_PI) * freq / opus_sample_rate;

    float phase = 0.0f;
    float chunk[chunk_frames];

    std::cout << "Sending 440Hz sine wave over UDP...\n";

    for (int i = 0; i < 1000; ++i)
    {
        for (int s = 0; s < chunk_frames; ++s)
        {
            chunk[s] = amplitude * std::sin(phase);
            phase   += phase_inc;
            if (phase > 2.0f * static_cast<float>(M_PI)) phase -= 2.0f * static_cast<float>(M_PI);
        }

        Packet packet {};
        packet.header.payload_type = PayloadType::PCM;
        packet.header.payload_size = chunk_frames * sizeof(float);
        std::memcpy(packet.payload, chunk, packet.header.payload_size);

        sender.send(packet);

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // give the receiver time to drain in-flight packets, then unblock its recvfrom
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;
    receiver.shutdown();
    rx.join();

    SDL_Quit();
    return 0;
}
