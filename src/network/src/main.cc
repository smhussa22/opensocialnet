// related headers

// c sys headers
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// cpp stdlib headers
#include <thread>
#include <atomic>
#include <array>
#include <iostream>
#include <string_view>

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
#include "LossSim.hh"

// Pull SIM_* env vars at startup. Empty or unparseable falls back to default
// so the sim stays bypassed unless the caller asked for it.
static double env_double(const char* name, double fallback)
{

    const char* value { std::getenv(name) };
    if (value == nullptr or *value == '\0') return fallback;
    try { return std::stod(value); } catch (...) { return fallback; }

}

static int env_int(const char* name, int fallback)
{

    const char* value { std::getenv(name) };
    if (value == nullptr or *value == '\0') return fallback;
    try { return std::stoi(value); } catch (...) { return fallback; }

}

static std::string env_str(const char* name, const char* fallback)
{

    const char* value { std::getenv(name) };
    return (value != nullptr and *value != '\0') ? std::string { value } : std::string { fallback };

}

// Print every recording / playback device SDL sees, then either pick the
// device whose name contains the substring `wanted` (case-insensitive,
// first match wins) or return 0 to mean "let SDL pick the default".
// Empty `wanted` ⇒ default device. Logs the chosen device by name so the
// user never has to guess which gadget OPus is actually talking to.
static SDL_AudioDeviceID pick_audio_device(bool recording, std::string_view wanted)
{

    int count { 0 };
    SDL_AudioDeviceID* ids { recording ? ::SDL_GetAudioRecordingDevices(&count) : ::SDL_GetAudioPlaybackDevices(&count) };
    const char* kind { recording ? "input" : "output" };
    if (ids == nullptr or count <= 0)
    {

        std::printf("[audio] no %s devices reported by SDL — falling back to default\n", kind);
        if (ids) ::SDL_free(ids);
        return recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

    }

    std::printf("[audio] %s devices (%d):\n", kind, count);
    SDL_AudioDeviceID chosen { 0 };
    std::string chosen_name { };
    for (int i { 0 }; i < count; ++i)
    {

        const char* name { ::SDL_GetAudioDeviceName(ids[i]) };
        const std::string nm { name ? name : "(unnamed)" };
        std::printf("  [%d] id=%u %s\n", i, static_cast<unsigned>(ids[i]), nm.c_str());

        if (chosen != 0 or wanted.empty()) continue;
        // case-insensitive substring match on the device name
        std::string lhs { nm }, rhs { wanted };
        for (auto& c : lhs) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto& c : rhs) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lhs.find(rhs) != std::string::npos) { chosen = ids[i]; chosen_name = nm; }

    }
    ::SDL_free(ids);

    if (chosen != 0)
    {

        std::printf("[audio] picked %s device by OSN_AUDIO_%s match: %s\n", kind, recording ? "INPUT" : "OUTPUT", chosen_name.c_str());
        return chosen;

    }

    if (!wanted.empty())
    {

        std::printf("[audio] no %s device name contained \"%.*s\"; falling back to default\n", kind, static_cast<int>(wanted.size()), wanted.data());

    }
    return recording ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

}

// 64-bit FNV-1a — fast enough at the packet rate, deterministic across
// hosts so the same room name hashes to the same room_id everywhere.
// Used so the relay can route by header alone without a side channel
// telling it the canonical room id.
static std::uint64_t fnv1a_64(std::string_view s) noexcept
{

    constexpr std::uint64_t prime  { 0x100000001b3ULL };
    constexpr std::uint64_t offset { 0xcbf29ce484222325ULL };
    std::uint64_t h { offset };
    for (const char c : s) { h ^= static_cast<std::uint8_t>(c); h *= prime; }
    return h;

}

// 32-bit FNV-1a for peer_id — same hash family, narrower output. We don't
// need cryptographic uniqueness; 32 bits is plenty for ≤10 peers per room.
static std::uint32_t fnv1a_32(std::string_view s) noexcept
{

    constexpr std::uint32_t prime  { 0x01000193u };
    constexpr std::uint32_t offset { 0x811c9dc5u };
    std::uint32_t h { offset };
    for (const char c : s) { h ^= static_cast<std::uint8_t>(c); h *= prime; }
    return h;

}

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

        // Enumerate + pick devices. OSN_AUDIO_INPUT / OSN_AUDIO_OUTPUT take a
        // case-insensitive substring of the device name (first match wins);
        // unset means "SDL default". The full device list always prints so
        // you can see what's available without staring at pavucontrol.
        const std::string         input_want_str  { env_str("OSN_AUDIO_INPUT",  "") };
        const std::string         output_want_str { env_str("OSN_AUDIO_OUTPUT", "") };
        const SDL_AudioDeviceID   playback_id     { pick_audio_device(/*recording=*/false, output_want_str) };
        const SDL_AudioDeviceID   recording_id    { pick_audio_device(/*recording=*/true,  input_want_str)  };

        PlaybackContext playback_context { &incoming_queue, &jitter_buffer, &decoder };
        SDL_AudioSpec spec { OpenSocialNet::Audio::create_opus_audio_spec() };
        OpenSocialNet::Audio::AudioStream audio_stream { spec, playback_id, on_playback, &playback_context };
        audio_stream.resume();
        std::cout << "[main] playback stream open\n";

        OpenSocialNet::Audio::AudioCapture capture {};
        if (!capture.init(recording_id))
        {
            std::cout << "[main] Failed to init capture\n";
            return 1;
        }
        std::cout << "[main] capture init ok\n";

        // Network endpoint config. Single-machine loopback uses the defaults
        // (talks to itself on 127.0.0.1:50100); pointing at the EC2 relay
        // means OSN_RELAY_HOST=<public ip> and OSN_LOCAL_PORT=<some free
        // local port>. Sender + receiver share one socket so the relay's
        // auto-learned src endpoint actually matches where this process is
        // recvfrom-ing.
        const std::string   relay_host { env_str("OSN_RELAY_HOST", "127.0.0.1") };
        const std::uint16_t relay_port { static_cast<std::uint16_t>(env_int("OSN_RELAY_PORT", 50100)) };
        const std::uint16_t local_port { static_cast<std::uint16_t>(env_int("OSN_LOCAL_PORT", 50100)) };

        OpenSocialNet::Network::UdpSender sender {};
        if (!sender.init(relay_host, relay_port, local_port))
        {
            std::printf("[main] Failed to init sender (host=%s port=%u local=%u)\n", relay_host.c_str(), relay_port, local_port);
            return 1;
        }
        std::printf("[main] sender init ok: sending to %s:%u from local :%u\n", relay_host.c_str(), relay_port, local_port);

        OpenSocialNet::Network::UdpReceiver receiver {};
        if (!receiver.init(sender.borrow_socket()))
        {
            std::cout << "[main] Failed to init receiver (shared socket not open)\n";
            return 1;
        }
        std::cout << "[main] receiver init ok (sharing sender's socket)\n";

        // Derive routing identity from env once at startup. Same string in =
        // same hash out everywhere, so two clients setting OSN_ROOM=general
        // end up stamping the same room_id and the relay groups them together.
        const std::string  room_str { env_str("OSN_ROOM", "loopback") };
        const std::string  user_str { env_str("OSN_USER", "self") };
        const std::uint64_t room_id { fnv1a_64(room_str) };
        const std::uint32_t peer_id { fnv1a_32(user_str) };
        sender.set_room_id(room_id);
        sender.set_peer_id(peer_id);
        std::printf("[main] routing identity room=\"%s\" (room_id=%016lx) user=\"%s\" (peer_id=%08x)\n",
            room_str.c_str(), static_cast<unsigned long>(room_id),
            user_str.c_str(), peer_id);

        // Adversarial sender-side conditions; all default to 0 (bypass).
        // SIM_LOSS_PCT=8 SIM_JITTER_MS=20 SIM_OOO_PCT=2 ./network
        OpenSocialNet::Network::LossSim sim {{
            .drop_pct      = env_double("SIM_LOSS_PCT", 0.0),
            .jitter_ms_max = env_int   ("SIM_JITTER_MS", 0  ),
            .ooo_pct       = env_double("SIM_OOO_PCT",  0.0),
        }};
        if (sim.enabled())
        {

            std::printf("[main] LossSim ACTIVE: drop=%.2f%% jitter_max=%dms ooo=%.2f%%\n",
                sim.config().drop_pct, sim.config().jitter_ms_max, sim.config().ooo_pct);

        }

        // Stamp at capture time so wire-level sequence reflects capture order;
        // LossSim's deferred sends then go via send_raw without re-stamping.
        const auto sim_send = [&sender](OpenSocialNet::Network::Packet p)
        {

            sender.send_raw(std::move(p));

        };

        std::thread rx { receive_thread, std::ref(receiver), std::ref(incoming_queue) };

        std::array<float, 480> chunk {};
        std::cout << "[main] starting capture loop...\n";

        int packets_sent = 0;

        // print a stats line every ~2s; the main loop ticks every 10ms.
        constexpr int stats_tick_interval { 200 };
        int ticks_since_stats { 0 };

        // Keepalive: send a zero-payload Packet at least every ~25s so the
        // relay's RoomTable doesn't expire this peer's endpoint and the
        // NAT mapping along the path doesn't time out either. Audio packets
        // reset the counter, so during active speech we never send any
        // extra. The relay forwards the zero-payload packet to other peers;
        // their receive_thread already drops payload_size==0.
        constexpr int keepalive_tick_interval { 2500 };
        int ticks_since_send { 0 };

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

                sender.stamp(packet);
                sim.submit(std::move(packet), sim_send);
                ++packets_sent;
                ticks_since_send = 0;

            }

            // Keepalive when audio's gone quiet for a while. Skips the LossSim
            // path on purpose — keepalives must actually reach the relay even
            // when SIM_LOSS_PCT=100 stress-tests are running.
            if (++ticks_since_send >= keepalive_tick_interval)
            {

                OpenSocialNet::Network::Packet keepalive { };
                keepalive.header.payload_type = OpenSocialNet::Network::PayloadType::Opus;
                keepalive.header.payload_size = 0;
                sender.send(keepalive);
                ticks_since_send = 0;

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