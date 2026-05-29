// audio + video multiplexed: one client sends/receives both over UDP
// audio on port 9000, video on port 9001 (separate jitter buffers, separate decode threads)

// audio headers
#include "AudioStream.hh"
#include "AudioCapture.hh"
#include "AudioConstants.hh"
#include "AudioEncoder.hh"
#include "AudioDecoder.hh"

// video headers
#include "VideoCapture.hh"
#include "VideoEncoder.hh"
#include "VideoDecoder.hh"
#include "VideoRenderer.hh"
#include "VideoPacketizer.hh"
#include "VideoReassembler.hh"

// c sys headers
#include <csignal>
#include <cstdint>
#include <cstdio>

// cpp stdlib headers
#include <array>
#include <atomic>
#include <random>
#include <thread>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "NetworkConstants.hh"
#include "Packet.hh"
#include "PacketJitterBuffer.hh"
#include "UdpReceiver.hh"
#include "UdpSender.hh"

static std::atomic<bool> running { true };

void on_signal(int) { running = false; }

void audio_receive_thread(OpenSocialNet::Network::UdpReceiver& receiver, OpenSocialNet::Network::PacketJitterBuffer& jitter_buffer)
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

void video_receive_thread(OpenSocialNet::Network::UdpReceiver& receiver, OpenSocialNet::Network::PacketJitterBuffer& jitter_buffer)
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

struct AudioPlaybackContext
{

    OpenSocialNet::Network::PacketJitterBuffer* jitter_buffer { };
    OpenSocialNet::Audio::AudioDecoder* decoder { };

};

static void on_audio_playback(void* userdata, SDL_AudioStream* stream, int additional, int total)
{

    auto* context { static_cast<AudioPlaybackContext*>(userdata) };
    OpenSocialNet::Network::Packet packet { };
    std::array<float, OpenSocialNet::Audio::samples_per_frame> decoded_pcm { };
    constexpr int decoded_byte_count { static_cast<int>(decoded_pcm.size() * sizeof(float)) };

    int fed { 0 };
    while (fed < additional)
    {

        if (!context->jitter_buffer->pop(packet)) break;

        const int decoded_samples { context->decoder->decode_bytes(std::span<const std::byte> { reinterpret_cast<const std::byte*>(packet.payload), packet.header.payload_size }, std::span<float> { decoded_pcm }) };
        if (decoded_samples < 0) continue;

        SDL_PutAudioStreamData(stream, decoded_pcm.data(), decoded_byte_count);
        fed += decoded_byte_count;

    }

}

int main()
{

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    if (!::SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO))
    {

        ::printf("ERROR: SDL_Init failed: %s\n", ::SDL_GetError());
        return 1;

    }

    int video_width { 640 };
    int video_height { 480 };

    ::printf("audio_video_client: audio on port 9000, video on port 9001\n");

    // audio setup
    OpenSocialNet::Network::PacketJitterBuffer audio_jitter_buffer { };
    OpenSocialNet::Audio::AudioDecoder audio_decoder { };
    if (!audio_decoder.valid()) { ::printf("audio decoder init failed\n"); return 1; }

    AudioPlaybackContext playback_context { &audio_jitter_buffer, &audio_decoder };
    SDL_AudioSpec audio_spec { OpenSocialNet::Audio::create_opus_audio_spec() };
    OpenSocialNet::Audio::AudioStream audio_stream { audio_spec, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, on_audio_playback, &playback_context };
    audio_stream.resume();

    OpenSocialNet::Audio::AudioCapture audio_capture { };
    if (!audio_capture.init()) { ::printf("audio capture init failed\n"); return 1; }

    OpenSocialNet::Audio::AudioEncoder audio_encoder { };
    if (!audio_encoder.valid()) { ::printf("audio encoder init failed\n"); return 1; }

    OpenSocialNet::Network::UdpReceiver audio_receiver { };
    if (!audio_receiver.init()) { ::printf("audio receiver init failed\n"); return 1; }

    OpenSocialNet::Network::UdpSender audio_sender { };
    if (!audio_sender.init()) { ::printf("audio sender init failed\n"); return 1; }

    std::thread audio_rx { audio_receive_thread, std::ref(audio_receiver), std::ref(audio_jitter_buffer) };

    // video setup
    OpenSocialNet::Network::PacketJitterBuffer video_jitter_buffer { };

    OpenSocialNet::Video::VideoCapture video_capture { };
    if (!video_capture.init("/dev/video0", video_width, video_height, 30)) { ::printf("video capture init failed\n"); return 1; }

    OpenSocialNet::Video::VideoEncoder video_encoder { };
    if (!video_encoder.init(video_width, video_height, 30)) { ::printf("video encoder init failed\n"); return 1; }

    OpenSocialNet::Video::VideoPacketizer video_packetizer { };
    std::mt19937 rng { std::random_device { }() };
    std::uint32_t ssrc { std::uniform_int_distribution<std::uint32_t> { }(rng) };
    video_packetizer.init(ssrc, 3000);

    OpenSocialNet::Video::VideoDecoder video_decoder { };
    if (!video_decoder.init()) { ::printf("video decoder init failed\n"); return 1; }

    OpenSocialNet::Video::VideoRenderer video_renderer { };
    if (!video_renderer.init(video_width, video_height, "Audio+Video Client")) { ::printf("video renderer init failed: %s\n", ::SDL_GetError()); return 1; }

    OpenSocialNet::Video::VideoReassembler video_reassembler { };

    OpenSocialNet::Network::UdpReceiver video_receiver { };
    if (!video_receiver.init(9001)) { ::printf("video receiver init failed\n"); return 1; }

    OpenSocialNet::Network::UdpSender video_sender { };
    if (!video_sender.init(OpenSocialNet::Network::ipv4_loopback_address, 9001)) { ::printf("video sender init failed\n"); return 1; }

    std::thread video_rx { video_receive_thread, std::ref(video_receiver), std::ref(video_jitter_buffer) };

    // buffers
    std::array<float, 480> audio_chunk { };
    std::size_t yuv_size { static_cast<std::size_t>(video_width * video_height) * 3 / 2 };
    std::vector<std::uint8_t> video_yuv(yuv_size);
    std::vector<std::byte> video_h264(yuv_size);

    int audio_packets_sent { 0 };
    int video_frames_sent { 0 };
    int video_packets_sent { 0 };
    int video_frames_rendered { 0 };

    ::printf("running. Close window or Ctrl-C to exit.\n\n");

    while (running)
    {

        ::SDL_Event ev { };
        while (::SDL_PollEvent(&ev))
        {

            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;

        }

        // send audio frames
        while (audio_capture.available() >= OpenSocialNet::Audio::samples_per_frame)
        {

            std::size_t got { audio_capture.read(std::span<float> { audio_chunk }) };
            if (got != OpenSocialNet::Audio::samples_per_frame) break;

            OpenSocialNet::Network::Packet audio_packet { };
            const int encoded_bytes { audio_encoder.encode_samples(std::span<const float> { audio_chunk.data(), got }, std::span<std::byte> { reinterpret_cast<std::byte*>(audio_packet.payload), OpenSocialNet::Network::maximum_packet_size }) };
            if (encoded_bytes <= 0) continue;

            audio_packet.header.payload_type = OpenSocialNet::Network::PayloadType::Opus;
            audio_packet.header.payload_size = static_cast<std::uint16_t>(encoded_bytes);

            audio_sender.send(audio_packet);
            ++audio_packets_sent;

        }

        // send video frames
        std::size_t captured_bytes { video_capture.capture_frame({ video_yuv.data(), yuv_size }) };
        if (captured_bytes > 0)
        {

            int h264_bytes { video_encoder.encode_frame(video_yuv.data(), video_width, { video_h264.data(), video_h264.size() }) };
            if (h264_bytes > 0)
            {

                auto packets { video_packetizer.packetize_frame({ video_h264.data(), static_cast<std::size_t>(h264_bytes) }) };
                for (auto& p : packets)
                {

                    if (video_sender.send_raw(p)) ++video_packets_sent;

                }
                ++video_frames_sent;

            }

        }

        // receive and render video frames
        OpenSocialNet::Network::Packet video_popped { };
        while (video_jitter_buffer.pop(video_popped))
        {

            if (!video_reassembler.feed(video_popped)) continue;

            auto frame_bytes { video_reassembler.take_complete_frame() };

            std::uint8_t* decoded_planes[3] { };
            int decoded_strides[3] { };
            if (!video_decoder.decode_packet(frame_bytes, decoded_planes, decoded_strides)) continue;

            const std::uint8_t* render_planes[3] { decoded_planes[0], decoded_planes[1], decoded_planes[2] };
            if (!video_renderer.render_frame(render_planes, decoded_strides, video_width, video_height))
            {

                ::printf("video render failed: %s\n", ::SDL_GetError());
                running = false;
                break;

            }

            ++video_frames_rendered;

        }

    }

    ::printf("\naudio: sent %d packets\nvideo: sent %d frames / %d packets, rendered %d frames\n", audio_packets_sent, video_frames_sent, video_packets_sent, video_frames_rendered);

    running = false;
    audio_receiver.shutdown();
    video_receiver.shutdown();
    audio_rx.join();
    video_rx.join();

    video_renderer.shutdown();
    ::SDL_Quit();

    return 0;

}
