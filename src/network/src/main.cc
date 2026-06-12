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
#include <algorithm>
#include <array>
#include <memory>
#include <iostream>
#include <string_view>
#include <vector>

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
#include "PeerMixer.hh"
#include "SpscQueue.hh"
#include "LossSim.hh"
#include "AudioPlc.hh"
#include "SignalingClient.hh"
#include "PeerVideoRouter.hh"
#include "VideoCapture.hh"
#include "VideoEncoder.hh"
#include "VideoPacketizer.hh"
#include "VideoPlc.hh"

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

// Video ring is wider: a keyframe burst is tens of fragments and several
// peers can keyframe in the same tick. Heap-allocated in main (≈500 KB).
using VideoQueue = OpenSocialNet::Network::SpscQueue<OpenSocialNet::Network::Packet, 256>;

static std::atomic<std::uint64_t> packets_dropped_overflow       { 0 }; // audio SpscQueue full at try_push
static std::atomic<std::uint64_t> video_packets_dropped_overflow { 0 }; // video SpscQueue full at try_push

void receive_thread(OpenSocialNet::Network::UdpReceiver& receiver, IncomingQueue& audio_queue, VideoQueue& video_queue)
{

    OpenSocialNet::Network::Packet packet {};

    while (running)
    {

        if (!receiver.receive(packet)) continue;
        if (packet.header.payload_size == 0) continue;
        if (packet.header.payload_size > OpenSocialNet::Network::maximum_packet_size) continue;

        // Demux by payload kind: video fragments go to the main loop's
        // router (decode + render on the main thread), everything else to
        // the audio callback's mixer.
        const bool is_video { packet.header.payload_type == OpenSocialNet::Network::PayloadType::H264 or packet.header.payload_type == OpenSocialNet::Network::PayloadType::H264_Screen };
        if (is_video)
        {

            if (!video_queue.try_push(packet)) video_packets_dropped_overflow.fetch_add(1, std::memory_order_relaxed);

        }
        else if (!audio_queue.try_push(packet)) packets_dropped_overflow.fetch_add(1, std::memory_order_relaxed);

    }

}

struct PlaybackContext
{

    IncomingQueue*                  incoming {}; // recv thread -> playback hand-off
    OpenSocialNet::Network::PeerMixer* mixer {}; // per-peer demux + jitter buffers + PLC + PCM mix

};

// Map "silence"/"repeat"/"opus"/"interp" (case-insensitive) to PlcStrategy.
// Unknown / empty values fall back to Opus — the best-quality default that
// requires no special configuration.
static OpenSocialNet::Plc::PlcStrategy parse_plc_strategy(std::string_view raw) noexcept
{

    std::string s { raw };
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "silence") return OpenSocialNet::Plc::PlcStrategy::Silence;
    if (s == "repeat" ) return OpenSocialNet::Plc::PlcStrategy::Repeat;
    if (s == "interp" ) return OpenSocialNet::Plc::PlcStrategy::Interpolate;
    return OpenSocialNet::Plc::PlcStrategy::Opus;

}

static const char* plc_strategy_name(OpenSocialNet::Plc::PlcStrategy s) noexcept
{

    switch (s)
    {

        case OpenSocialNet::Plc::PlcStrategy::Silence:     return "silence";
        case OpenSocialNet::Plc::PlcStrategy::Repeat:      return "repeat";
        case OpenSocialNet::Plc::PlcStrategy::Opus:        return "opus";
        case OpenSocialNet::Plc::PlcStrategy::Interpolate: return "interp";

    }
    return "?";

}

// Map "skip"/"hold"/"interp"/"motion" (case-insensitive) to
// VideoPlcStrategy. Unknown / empty falls back to MotionCompensated —
// the best-looking concealer and cheap at call resolutions.
static OpenSocialNet::Plc::VideoPlcStrategy parse_video_plc_strategy(std::string_view raw) noexcept
{

    std::string s { raw };
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "skip"  ) return OpenSocialNet::Plc::VideoPlcStrategy::Skip;
    if (s == "hold"  ) return OpenSocialNet::Plc::VideoPlcStrategy::Hold;
    if (s == "interp") return OpenSocialNet::Plc::VideoPlcStrategy::Interpolate;
    return OpenSocialNet::Plc::VideoPlcStrategy::MotionCompensated;

}

static const char* video_plc_strategy_name(OpenSocialNet::Plc::VideoPlcStrategy s) noexcept
{

    switch (s)
    {

        case OpenSocialNet::Plc::VideoPlcStrategy::Skip:              return "skip";
        case OpenSocialNet::Plc::VideoPlcStrategy::Hold:              return "hold";
        case OpenSocialNet::Plc::VideoPlcStrategy::Interpolate:       return "interp";
        case OpenSocialNet::Plc::VideoPlcStrategy::MotionCompensated: return "motion";

    }
    return "?";

}

// Stand-in camera for headless boxes (WSL guests, EC2 test clients): a
// scrolling diagonal gradient + bouncing bright square in YUV420P,
// deterministic per frame index so two runs produce identical streams.
struct SyntheticVideoSource
{

    int           width       { 0 }; // luma width
    int           height      { 0 }; // luma height
    std::uint64_t frame_index { 0 }; // advances per fill(); drives the animation

    void fill(std::vector<std::uint8_t>& yuv) noexcept
    {

        const int t { static_cast<int>(frame_index % 100000) };
        std::uint8_t* y_plane { yuv.data() };
        std::uint8_t* u_plane { y_plane + static_cast<std::size_t>(width) * height };
        std::uint8_t* v_plane { u_plane + static_cast<std::size_t>(width / 2) * (height / 2) };

        // scrolling gradient background
        for (int y { 0 }; y < height; ++y)
        {

            for (int x { 0 }; x < width; ++x) y_plane[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint8_t>((x + y + 2 * t) & 0xff);

        }

        // bouncing bright square so motion estimation has something to chase
        const int square { 32 };
        const int px { (10 + 3 * t) % (width - square) };
        const int py { (10 + 2 * t) % (height - square) };
        for (int y { 0 }; y < square; ++y)
        {

            for (int x { 0 }; x < square; ++x) y_plane[static_cast<std::size_t>(py + y) * width + px + x] = 235;

        }

        // slowly drifting chroma so the picture isn't grayscale
        const std::size_t chroma_size { static_cast<std::size_t>(width / 2) * (height / 2) };
        std::fill(u_plane, u_plane + chroma_size, static_cast<std::uint8_t>(128 + (t / 4) % 64));
        std::fill(v_plane, v_plane + chroma_size, static_cast<std::uint8_t>(128));

        ++frame_index;

    }

};

static void on_playback(void* userdata, SDL_AudioStream* stream, int additional, int total)
{

    auto* context = static_cast<PlaybackContext*>(userdata);
    OpenSocialNet::Network::Packet packet {};
    std::array<float, OpenSocialNet::Audio::samples_per_frame> mixed_pcm {};
    constexpr int mixed_byte_count { static_cast<int>(mixed_pcm.size() * sizeof(float)) };

    // drain everything the recv thread parked since the last callback into
    // each source's jitter buffer (the mixer demuxes by peer_id + ssrc).
    while (context->incoming->try_pop(packet)) context->mixer->route(packet);

    // One mixed frame at a time. Zero contributors means every source is
    // dry AND past its PLC budget — break so SDL silence-fills the rest.
    int fed = 0;
    while (fed < additional)
    {

        if (context->mixer->mix_one_frame(std::span<float> { mixed_pcm }) == 0) break;

        SDL_PutAudioStreamData(stream, mixed_pcm.data(), mixed_byte_count);
        fed += mixed_byte_count;

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

        // Video ring lives on the heap — 256 Packets is ~500 KB, too fat
        // for the stack next to the audio ring.
        auto video_queue { std::make_unique<VideoQueue>() };

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

        // PLC strategy chosen at startup, stamped into every per-peer PLC
        // the mixer creates. Default is opus (best quality, no extra
        // configuration needed); override with OSN_PLC=silence|repeat|interp.
        const OpenSocialNet::Plc::PlcStrategy plc_strategy { parse_plc_strategy(env_str("OSN_PLC", "opus")) };
        std::printf("[main] PLC strategy=%s\n", plc_strategy_name(plc_strategy));

        // Adaptive playout: OSN_ADAPTIVE_JB=1 grows / shrinks each source's
        // jitter-buffer playout threshold based on observed RFC 3550 jitter.
        // Off by default — flip to 1 to see the threshold drift in
        // [net-stats] as network conditions change.
        const bool adaptive_jb { env_int("OSN_ADAPTIVE_JB", 0) != 0 };
        if (adaptive_jb) std::printf("[main] adaptive jitter buffers ENABLED (bounds 1..20 frames, recompute every 50 pops)\n");

        // Per-peer receive state lives here: one jitter buffer + opus
        // decoder + PLC per inbound (peer_id, ssrc), mixed to one stream.
        OpenSocialNet::Network::PeerMixer mixer { plc_strategy, adaptive_jb };

        PlaybackContext playback_context { &incoming_queue, &mixer };
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

        // Network endpoint config. Three modes, picked at startup:
        //
        //   1. OSN_SIGNALING_HOST is set → real signaling handshake. The
        //      client opens a WS to the gateway, authenticates with
        //      OSN_USER + OSN_AUTH_TOKEN, sends JoinVoice(OSN_ROOM), and
        //      uses the relay endpoint the server replies with. The user's
        //      OSN_RELAY_HOST is ignored in this mode (the server is the
        //      source of truth for where the relay lives).
        //
        //   2. OSN_RELAY_HOST is set → direct relay mode. Skip signaling,
        //      use the literal IP from the env var. Original behaviour;
        //      kept so loopback / pre-deploy testing still works without
        //      a running signaling server.
        //
        //   3. Neither set → loopback to 127.0.0.1. The original default.
        //
        // Sender + receiver share one socket so the relay's auto-learned
        // src endpoint actually matches where this process is recvfrom-ing.
        std::string   relay_host { env_str("OSN_RELAY_HOST", "127.0.0.1") };
        std::uint16_t relay_port { static_cast<std::uint16_t>(env_int("OSN_RELAY_PORT", 50100)) };
        const std::uint16_t local_port { static_cast<std::uint16_t>(env_int("OSN_LOCAL_PORT", 50100)) };
        std::uint32_t assigned_ssrc { 0 };

        OpenSocialNet::Network::SignalingClient signaling { };
        const std::string signaling_host { env_str("OSN_SIGNALING_HOST", "") };
        if (!signaling_host.empty())
        {

            const std::uint16_t signaling_port { static_cast<std::uint16_t>(env_int("OSN_SIGNALING_PORT", 9001)) };
            const std::string   signaling_path { env_str("OSN_SIGNALING_PATH", "/gateway") };
            const std::string   user_id        { env_str("OSN_USER",           "self") };
            const std::string   auth_token     { env_str("OSN_AUTH_TOKEN",     "") };
            const std::string   channel_id     { env_str("OSN_ROOM",           "loopback") };

            std::printf("[main] signaling: ws://%s:%u%s user=%s channel=%s\n", signaling_host.c_str(), signaling_port, signaling_path.c_str(), user_id.c_str(), channel_id.c_str());

            if (!signaling.connect_and_hello(signaling_host, signaling_port, signaling_path, user_id, auth_token))
            {

                std::fprintf(stderr, "[main] signaling Hello failed; aborting\n");
                return 1;

            }

            OpenSocialNet::Network::VoicePeerInfo                 self_peer { };
            std::vector<OpenSocialNet::Network::VoicePeerInfo>    other_peers { };
            if (!signaling.join_voice(channel_id, self_peer, other_peers))
            {

                std::fprintf(stderr, "[main] signaling JoinVoice failed; aborting\n");
                return 1;

            }

            relay_host    = self_peer.ip;
            relay_port    = self_peer.port;
            assigned_ssrc = self_peer.ssrc;

            // Kick off the background reader so VoicePeerJoined / Left
            // for peers who arrive AFTER us show up in the [net-stats]
            // line below. Heartbeats keep the gateway's 120s idleTimeout
            // from dropping us mid-call; we send them from the main
            // loop's tick further down.
            signaling.start_event_reader();

        }

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
        sender.set_ssrc(assigned_ssrc);
        std::printf("[main] routing identity room=\"%s\" (room_id=%016lx) user=\"%s\" (peer_id=%08x) ssrc=%u\n",
            room_str.c_str(), static_cast<unsigned long>(room_id),
            user_str.c_str(), peer_id, assigned_ssrc);

        // ---- video: camera (or synthetic stand-in) -> x264 -> Packets ----
        //
        // OSN_VIDEO=1 turns the send side on. The receive side (router +
        // per-peer decode/PLC/render) is ALWAYS live so a voice-only
        // client still shows peers who do send video. OSN_VIDEO_RENDER=0
        // keeps everything headless (decode + conceal, no windows) for
        // CI / EC2 test clients.
        const bool video_enabled { env_int("OSN_VIDEO", 0) != 0 };
        const int  video_fps     { std::clamp(env_int("OSN_VIDEO_FPS", 15), 1, 60) };
        int video_width  { env_int("OSN_VIDEO_W", 320) };
        int video_height { env_int("OSN_VIDEO_H", 240) };

        bool render_ok { false };
        if (env_int("OSN_VIDEO_RENDER", 1) != 0)
        {

            render_ok = ::SDL_InitSubSystem(SDL_INIT_VIDEO);
            if (!render_ok) std::printf("[main] SDL video subsystem unavailable (%s) — remote video will decode headless\n", ::SDL_GetError());

        }

        const OpenSocialNet::Plc::VideoPlcStrategy video_plc_strategy { parse_video_plc_strategy(env_str("OSN_VIDEO_PLC", "motion")) };
        OpenSocialNet::Network::PeerVideoRouter video_router { video_plc_strategy, render_ok };
        std::printf("[main] video PLC strategy=%s render=%d\n", video_plc_strategy_name(video_plc_strategy), render_ok ? 1 : 0);

        OpenSocialNet::Video::VideoCapture video_capture {};
        OpenSocialNet::Video::VideoEncoder video_encoder {};
        OpenSocialNet::Video::VideoPacketizer video_packetizer {};
        SyntheticVideoSource synthetic_video {};
        bool use_camera { false };
        bool video_send { false };
        std::vector<std::uint8_t> video_yuv {};
        std::vector<std::byte> video_h264 {};

        if (video_enabled)
        {

            const std::string video_device { env_str("OSN_VIDEO_DEVICE", "/dev/video0") };
            use_camera = video_capture.init(video_device.c_str(), video_width, video_height, video_fps);
            if (use_camera)
            {

                video_width = video_capture.width();
                video_height = video_capture.height();
                std::printf("[main] camera %s open at %dx%d@%d\n", video_device.c_str(), video_width, video_height, video_fps);

            }
            else
            {

                synthetic_video.width = video_width;
                synthetic_video.height = video_height;
                std::printf("[main] no camera at %s — synthetic video source %dx%d@%d\n", video_device.c_str(), video_width, video_height, video_fps);

            }

            video_send = video_encoder.init(video_width, video_height, video_fps);
            if (video_send)
            {

                // Camera stream ssrc: the signaling-assigned (audio) ssrc
                // with the high bit set, so mic + cam from the same peer
                // demux into distinct receive lanes. Falls back to peer_id
                // when there was no signaling round-trip (loopback mode).
                const std::uint32_t video_ssrc { (assigned_ssrc != 0 ? assigned_ssrc : peer_id) | 0x80000000u };
                video_packetizer.init(video_ssrc, 90000u / static_cast<std::uint32_t>(video_fps));
                video_yuv.resize(static_cast<std::size_t>(video_width) * video_height * 3 / 2);
                video_h264.resize(video_yuv.size());
                std::printf("[main] video send on: ssrc=%u %dx%d@%d\n", video_ssrc, video_width, video_height, video_fps);

            }
            else std::printf("[main] x264 init failed — video send disabled\n");

        }

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

        std::thread rx { receive_thread, std::ref(receiver), std::ref(incoming_queue), std::ref(*video_queue) };

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

        // Signaling heartbeat. The gateway's uWS::App idleTimeout is 120s
        // — without traffic the WS gets dropped mid-call. 60s gives us a
        // comfy margin. Cheap, since the WS reader thread is already
        // open; this is just one envelope every 6000 ticks.
        constexpr int signaling_heartbeat_tick_interval { 6000 };
        int ticks_since_heartbeat { 0 };

        // Mixer source reap cadence (~5s at the 10ms tick).
        constexpr int reap_tick_interval { 500 };
        int ticks_since_reap { 0 };

        // Video frame pacing off the same 10ms tick: 15fps = every ~6 ticks.
        const int video_tick_interval { std::max(1, 100 / video_fps) };
        int ticks_since_video { 0 };
        std::uint64_t video_frames_sent { 0 };
        std::uint64_t video_packets_sent { 0 };

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

            // Video send: encode + ship one frame when a source has one.
            // The camera is non-blocking and paces itself (poll every tick,
            // EAGAIN means not ready yet); the synthetic source is paced by
            // the tick counter. Encoded video feeds the keepalive timer too.
            if (video_send)
            {

                bool have_frame { false };
                if (use_camera) have_frame = video_capture.capture_frame(std::span<std::uint8_t> { video_yuv }) > 0;
                else if (++ticks_since_video >= video_tick_interval)
                {

                    ticks_since_video = 0;
                    synthetic_video.fill(video_yuv);
                    have_frame = true;

                }

                if (have_frame)
                {

                    const int encoded { video_encoder.encode_frame(video_yuv.data(), video_width, std::span<std::byte> { video_h264 }) };
                    if (encoded > 0)
                    {

                        auto packets { video_packetizer.packetize_frame(std::span<const std::byte> { video_h264.data(), static_cast<std::size_t>(encoded) }) };
                        for (auto& p : packets)
                        {

                            p.header.room_id = room_id;
                            p.header.peer_id = peer_id;
                            sim.submit(std::move(p), sim_send);
                            ++video_packets_sent;

                        }
                        ++video_frames_sent;
                        ticks_since_send = 0;

                    }

                }

            }

            // Drain inbound video into the per-peer router, then decode +
            // conceal + render whatever became complete. Always runs —
            // voice-only clients still show peers' video.
            {

                OpenSocialNet::Network::Packet vp {};
                while (video_queue->try_pop(vp)) video_router.route(vp);
                video_router.pump();

            }

            // Keep the SDL windows responsive (close button, expose, move).
            if (render_ok)
            {

                SDL_Event ev;
                while (::SDL_PollEvent(&ev)) {}

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

            // Signaling heartbeat — only if we actually went through the
            // gateway. The WebSocket connection is otherwise idle as far
            // as uWS can tell, since all audio traffic is on a separate
            // UDP socket.
            if (!signaling_host.empty() and ++ticks_since_heartbeat >= signaling_heartbeat_tick_interval)
            {

                signaling.send_heartbeat();
                ticks_since_heartbeat = 0;

            }

            // Reap mixer sources that went silent — their peer left the
            // room or lost connectivity. 10s is generous: live peers send
            // continuously (mic frames or 25s keepalives never reach the
            // mixer, but real audio does at 100 pkt/s).
            if (++ticks_since_reap >= reap_tick_interval)
            {

                mixer.reap_idle(std::chrono::seconds { 10 });
                video_router.reap_idle(std::chrono::seconds { 10 });
                ticks_since_reap = 0;

            }

            if (++ticks_since_stats >= stats_tick_interval)
            {

                const auto agg { mixer.stats() };
                const std::size_t live_peers { signaling_host.empty() ? std::size_t { 0 } : signaling.peers().size() };
                std::printf("[net-stats] src=%zu obs=%llu lost=%llu ooo=%llu jitter_ms=%.2f spsc=%zu jb=%zu jb_th=%zu jb_adapts=%llu drop_overflow=%llu sent=%d plc=%llu peers=%zu\n",
                    agg.sources,
                    static_cast<unsigned long long>(agg.packets_observed),
                    static_cast<unsigned long long>(agg.packets_lost),
                    static_cast<unsigned long long>(agg.packets_out_of_order),
                    agg.max_jitter_ms,
                    incoming_queue.size(),
                    agg.buffered_packets,
                    agg.max_playout_threshold,
                    static_cast<unsigned long long>(agg.adaptations),
                    static_cast<unsigned long long>(packets_dropped_overflow.load(std::memory_order_relaxed)),
                    packets_sent,
                    static_cast<unsigned long long>(agg.concealments),
                    live_peers);

                const auto vagg { video_router.stats() };
                if (vagg.sources > 0 or video_send)
                {

                    std::printf("[vid-stats] src=%zu obs=%llu lost=%llu ooo=%llu jb=%zu dec=%llu plc=%llu dropped=%llu missed=%llu rendered=%llu sent_frames=%llu sent_pkts=%llu drop_overflow=%llu\n",
                        vagg.sources,
                        static_cast<unsigned long long>(vagg.packets_observed),
                        static_cast<unsigned long long>(vagg.packets_lost),
                        static_cast<unsigned long long>(vagg.packets_out_of_order),
                        vagg.buffered_packets,
                        static_cast<unsigned long long>(vagg.frames_decoded),
                        static_cast<unsigned long long>(vagg.frames_concealed),
                        static_cast<unsigned long long>(vagg.frames_dropped),
                        static_cast<unsigned long long>(vagg.frames_missed),
                        static_cast<unsigned long long>(vagg.frames_rendered),
                        static_cast<unsigned long long>(video_frames_sent),
                        static_cast<unsigned long long>(video_packets_sent),
                        static_cast<unsigned long long>(video_packets_dropped_overflow.load(std::memory_order_relaxed)));

                }
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

    } // audio_stream + mixer + sender + receiver destroyed here, before SDL_Quit

    SDL_Quit();
    return 0;

}