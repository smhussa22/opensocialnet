// related headers

// c sys headers
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

// cpp stdlib headers
#include <thread>
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <iostream>
#include <string>
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
#include "IceTransport.hh"
#include "RttProber.hh"
#include "AudioPlc.hh"
#include "SignalingClient.hh"
#include "PeerVideoRouter.hh"
#include "ScreenCapture.hh"
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

// Runtime media controls toggled by signals from a parent (native_client).
// SIGUSR1 toggles audio mute, SIGUSR2 toggles video send, SIGURG toggles
// screenshare send. SIGURG over SIGHUP because terminals send SIGHUP when
// closed — we don't want a terminal close to accidentally fire a toggle.
// Encoders stay initialised across toggles so flipping back on costs nothing.
static std::atomic<bool> audio_muted  { false };
static std::atomic<bool> video_active { true  };
static std::atomic<bool> screen_active{ false };

static void on_signal(int)
{
    running = false;
}

static void on_audio_toggle (int) { audio_muted  = !audio_muted .load(); }
static void on_video_toggle (int) { video_active = !video_active.load(); }
static void on_screen_toggle(int) { screen_active = !screen_active.load(); }

// SPSC ring sized for ~1.28s of headroom at 100 packets/s (Opus 10ms frames).
// Power of two so SpscQueue's mask wrap-around fires.
using IncomingQueue = OpenSocialNet::Network::SpscQueue<OpenSocialNet::Network::Packet, 128>;

// Video ring is wider: a keyframe burst is tens of fragments and several
// peers can keyframe in the same tick. Heap-allocated in main (≈500 KB).
using VideoQueue = OpenSocialNet::Network::SpscQueue<OpenSocialNet::Network::Packet, 256>;

static std::atomic<std::uint64_t> packets_dropped_overflow       { 0 }; // audio SpscQueue full at try_push
static std::atomic<std::uint64_t> video_packets_dropped_overflow { 0 }; // video SpscQueue full at try_push

void receive_thread(OpenSocialNet::Network::UdpReceiver& receiver, IncomingQueue& audio_queue, VideoQueue& video_queue,
                    OpenSocialNet::Network::UdpSender& sender, OpenSocialNet::Network::RttProber& rtt_prober, std::uint32_t self_peer_id)
{

    OpenSocialNet::Network::Packet packet {};

    while (running)
    {

        if (!receiver.receive(packet)) continue;
        if (packet.header.payload_size == 0) continue;
        if (packet.header.payload_size > OpenSocialNet::Network::maximum_packet_size) continue;

        // RTT lane: intercepted before any queue so pings/pongs never
        // touch the audio jitter buffer or the video router. Pong is
        // sent back inline — it's just a 32+8 byte stamp+sendto.
        if (packet.header.payload_type == OpenSocialNet::Network::PayloadType::RttPing)
        {

            sender.send_raw(rtt_prober.build_pong(packet, self_peer_id));
            continue;

        }
        if (packet.header.payload_type == OpenSocialNet::Network::PayloadType::RttPong)
        {

            rtt_prober.on_pong(packet.header.peer_id, packet, std::chrono::steady_clock::now());
            continue;

        }

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
    // Unbuffered stdout — when launched as a child of native_client our
    // stdout is a regular file (not a tty), and full buffering would hide
    // every [main]/[net-stats] line until 4KB had accumulated.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGUSR1, on_audio_toggle);
    std::signal(SIGUSR2, on_video_toggle);
    std::signal(SIGURG,  on_screen_toggle);

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

        // RTT prober — 1Hz ping to the room, EWMA of returning pongs per
        // remote peer. Lives next to the mixer so its lifetime tracks the
        // session. Receive thread + ICE recv callback both reach into it.
        OpenSocialNet::Network::RttProber rtt_prober { std::chrono::milliseconds { 1000 } };

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

        // OSN_ICE=1 negotiates a direct peer-to-peer lane via libnice
        // after signaling, instead of sending media through the relay.
        // The mailbox below is filled by the signaling reader thread when
        // the remote peer's SDP arrives over the chat path; the
        // negotiation loop further down polls it.
        const bool  ice_enabled { env_int("OSN_ICE", 0) != 0 };
        std::mutex  ice_mailbox_mu { };
        std::string ice_remote_user { };
        std::string ice_remote_sdp { };

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

            // ICE SDP exchange rides the chat path: cache the latest
            // "OSN-ICE1|<sdp>" message from the remote peer so the
            // negotiation loop below can pick it up. 1:1 only for now —
            // a later peer's SDP simply overwrites an earlier one.
            if (ice_enabled) signaling.set_chat_handler([&ice_mailbox_mu, &ice_remote_user, &ice_remote_sdp](const std::string& sender_id, const std::string& content)
            {

                if (!content.starts_with("OSN-ICE1|")) return;
                std::scoped_lock<std::mutex> lock { ice_mailbox_mu };
                ice_remote_user = sender_id;
                ice_remote_sdp  = content.substr(sizeof("OSN-ICE1|") - 1);

            });

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

        // Per-session stats logs — gitignored ./logs/ at the repo root,
        // filenames embed start time + user so each run is identifiable
        // after the fact. Two outputs: a .log mirroring the current
        // [net-stats]/[vid-stats] format for at-a-glance reading, and a
        // .csv for plotting (one row per stats tick). stdout stays clean
        // of stats; other diagnostic prints ([main], [ice], …) still go
        // there because they matter for setup / failure triage.
        std::filesystem::create_directories("logs");
        const auto session_clock_start { std::chrono::system_clock::now() };
        const auto session_t_c         { std::chrono::system_clock::to_time_t(session_clock_start) };
        std::tm    session_tm          { };
        ::localtime_r(&session_t_c, &session_tm);
        char ts_buf[32] { };
        std::strftime(ts_buf, sizeof ts_buf, "%Y-%m-%d_%H%M%S", &session_tm);
        const std::string log_basename { std::string { "logs/" } + ts_buf + "_" + user_str };
        const std::string log_path     { log_basename + ".log" };
        const std::string csv_path     { log_basename + ".csv" };
        FILE* stats_log_fp { std::fopen(log_path.c_str(), "w") };
        FILE* stats_csv_fp { std::fopen(csv_path.c_str(), "w") };
        if (stats_log_fp == nullptr or stats_csv_fp == nullptr)
        {

            std::printf("[main] WARN: could not open stats files (%s / %s) — stats will be silently dropped this session\n", log_path.c_str(), csv_path.c_str());

        }
        else
        {

            // Line-buffered so 'tail -f logs/...log' shows every tick the
            // moment it lands without needing fflush calls at every site.
            std::setvbuf(stats_log_fp, nullptr, _IOLBF, 0);
            std::setvbuf(stats_csv_fp, nullptr, _IOLBF, 0);
            std::fprintf(stats_csv_fp,
                "elapsed_s,sources,packets_observed,packets_lost,packets_ooo,jitter_ms,"
                "spsc_size,jb_buffered,jb_threshold,jb_adapts,drop_overflow,sent,plc,peers,"
                "vid_sources,vid_observed,vid_lost,vid_ooo,vid_jb,vid_decoded,vid_concealed,"
                "vid_dropped,vid_missed,vid_rendered,vid_sent_frames,vid_sent_pkts,"
                "scr_frames,scr_pkts,vid_drop_overflow,rtt_summary\n");
            std::printf("[main] stats -> %s + %s (stdout suppressed for [net-stats]/[vid-stats])\n", log_path.c_str(), csv_path.c_str());

        }
        const auto session_steady_start { std::chrono::steady_clock::now() };

        // ---- ICE: peer-to-peer media lane via libnice (OSN_ICE=1) ----
        //
        // Connectivity only: the exact same stamped Packets ride the
        // nominated candidate pair instead of the relay socket. SDP goes
        // over the signaling chat path, so this needs signaling mode. Any
        // failure falls back to the relay path picked above.
        OpenSocialNet::Network::IceTransport ice { };
        bool ice_active { false };
        if (ice_enabled and signaling_host.empty()) std::printf("[main] OSN_ICE=1 needs OSN_SIGNALING_HOST for the SDP exchange — staying on the relay path\n");
        else if (ice_enabled)
        {

            OpenSocialNet::Network::IceConfig ice_config { };
            ice_config.stun_host = env_str("OSN_STUN_HOST", "");
            ice_config.stun_port = static_cast<std::uint16_t>(env_int("OSN_STUN_PORT", 3478));
            ice_config.turn_host = env_str("OSN_TURN_HOST", "");
            ice_config.turn_port = static_cast<std::uint16_t>(env_int("OSN_TURN_PORT", 3478));
            ice_config.turn_user = env_str("OSN_TURN_USER", "");
            ice_config.turn_pass = env_str("OSN_TURN_PASS", "");
            ice_config.force_relay = env_int("OSN_ICE_FORCE_RELAY", 0) != 0;

            // Inbound ICE datagrams take receive_thread's place as the
            // single producer for both SPSC rings — the relay rx thread
            // is not spawned when the ICE lane wins. Runs on the GLib
            // loop thread.
            const auto ice_recv = [&incoming_queue, &video_queue, &ice, &rtt_prober, peer_id](std::span<const std::uint8_t> datagram)
            {

                OpenSocialNet::Network::Packet packet { };
                if (datagram.size() > sizeof(packet)) return;
                std::memcpy(&packet, datagram.data(), datagram.size());
                if (!OpenSocialNet::Network::packet_from_wire(packet, datagram.size())) return;
                if (packet.header.payload_size == 0) return;

                // RTT lane on the ICE path — same as the relay receive
                // thread: intercept before any queue, echo the Pong
                // straight back over the nominated candidate pair.
                if (packet.header.payload_type == OpenSocialNet::Network::PayloadType::RttPing)
                {

                    auto pong { rtt_prober.build_pong(packet, peer_id) };
                    ice.send(OpenSocialNet::Network::packet_to_wire(pong));
                    return;

                }
                if (packet.header.payload_type == OpenSocialNet::Network::PayloadType::RttPong)
                {

                    rtt_prober.on_pong(packet.header.peer_id, packet, std::chrono::steady_clock::now());
                    return;

                }

                const bool is_video { packet.header.payload_type == OpenSocialNet::Network::PayloadType::H264 or packet.header.payload_type == OpenSocialNet::Network::PayloadType::H264_Screen };
                if (is_video)
                {

                    if (!video_queue->try_push(packet)) video_packets_dropped_overflow.fetch_add(1, std::memory_order_relaxed);

                }
                else if (!incoming_queue.try_push(packet)) packets_dropped_overflow.fetch_add(1, std::memory_order_relaxed);

            };

            if (!ice.init(ice_config, ice_recv)) std::printf("[main] ICE init failed — staying on the relay path\n");
            else if (!ice.wait_gathered(10'000)) std::printf("[main] ICE gathering timed out — staying on the relay path\n");
            else
            {

                // Publish our SDP, then wait for the remote one — both
                // sides do the same dance. Re-publish every ~2s so a peer
                // that subscribed after our first send still gets it.
                const std::string local_blob { "OSN-ICE1|" + ice.local_description() };
                signaling.send_chat(room_str, local_blob);
                std::printf("[main] ICE: local SDP published to channel \"%s\", waiting for a peer (60s)...\n", room_str.c_str());

                std::string remote_user { };
                std::string remote_sdp { };
                const auto ice_deadline { std::chrono::steady_clock::now() + std::chrono::seconds { 60 } };
                int polls { 0 };
                while (running and remote_sdp.empty() and std::chrono::steady_clock::now() < ice_deadline)
                {

                    {

                        std::scoped_lock<std::mutex> lock { ice_mailbox_mu };
                        remote_user = ice_remote_user;
                        remote_sdp  = ice_remote_sdp;

                    }
                    if (remote_sdp.empty())
                    {

                        if (++polls % 20 == 0) signaling.send_chat(room_str, local_blob);
                        std::this_thread::sleep_for(std::chrono::milliseconds { 100 });

                    }

                }

                // The peer's SDP arriving proves they joined (and thus
                // subscribed) AFTER some of our earlier publishes may have
                // fanned out to nobody — publish once more so a peer that
                // missed those definitely gets ours.
                if (!remote_sdp.empty()) signaling.send_chat(room_str, local_blob);

                // Deterministic role tie-break: lower user id controls.
                if (remote_sdp.empty()) std::printf("[main] ICE: no peer SDP arrived — staying on the relay path\n");
                else if (!ice.start_checks(remote_sdp, user_str < remote_user)) std::printf("[main] ICE: remote SDP rejected — staying on the relay path\n");
                else if (!ice.wait_ready(15'000)) std::printf("[main] ICE: connectivity checks failed — staying on the relay path\n");
                else
                {

                    ice_active = true;
                    std::printf("[main] ICE CONNECTED to %s — media flows peer-to-peer, relay bypassed\n", remote_user.c_str());

                }

            }

            if (!ice_active) ice.shutdown();

        }

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

            // Retry with a beat between attempts: on WSL/usbipd the device
            // can linger busy for a moment after a previous client was
            // killed mid-stream.
            for (int attempt { 0 }; attempt < 3 and !use_camera; ++attempt)
            {

                if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds { 300 });
                use_camera = video_capture.init(video_device.c_str(), video_width, video_height, video_fps);

            }

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

                // The recurring WSL failure mode: the shell predates the
                // user's 'video' group membership, so the device node is
                // there but unreadable.
                if (::access(video_device.c_str(), R_OK | W_OK) != 0 and errno == EACCES) std::printf("[main] %s: permission denied — this shell may predate your 'video' group membership; run 'newgrp video' or open a fresh terminal\n", video_device.c_str());

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

        // ---- screenshare: X11 root grab -> x264 -> H264_Screen Packets ----
        //
        // OSN_SCREEN=1 turns it on, independent of OSN_VIDEO so a peer can
        // share their screen without a camera. Same encoder/packetizer
        // shape as the camera lane, just a different payload type + ssrc
        // bit so receivers demux it into its own window.
        const bool screen_enabled { env_int("OSN_SCREEN", 0) != 0 };
        const int  screen_fps     { std::clamp(env_int("OSN_SCREEN_FPS", 10), 1, 30) };
        OpenSocialNet::Video::ScreenCapture screen_capture {};
        OpenSocialNet::Video::VideoEncoder screen_encoder {};
        OpenSocialNet::Video::VideoPacketizer screen_packetizer {};
        bool screen_send { false };
        std::vector<std::uint8_t> screen_yuv {};
        std::vector<std::byte> screen_h264 {};

        if (screen_enabled)
        {

            // OSN_SCREEN_WINDOW=0x<id> grabs that X11 window (xwininfo
            // shows ids); 0/unset = auto-latch onto the largest window.
            const unsigned long screen_window { std::strtoul(env_str("OSN_SCREEN_WINDOW", "0").c_str(), nullptr, 0) };
            if (screen_capture.init(env_int("OSN_SCREEN_W", 640), env_int("OSN_SCREEN_H", 360), screen_window))
            {

                const int screen_width { screen_capture.width() };
                const int screen_height { screen_capture.height() };
                screen_send = screen_encoder.init(screen_width, screen_height, screen_fps);
                if (screen_send)
                {

                    // Screen lane ssrc: bit 30 marks screenshare the way
                    // bit 31 marks camera, so mic + cam + screen from one
                    // peer land in three distinct receive lanes.
                    const std::uint32_t screen_ssrc { (assigned_ssrc != 0 ? assigned_ssrc : peer_id) | 0x40000000u };
                    screen_packetizer.init(screen_ssrc, 90000u / static_cast<std::uint32_t>(screen_fps), OpenSocialNet::Network::PayloadType::H264_Screen);
                    screen_yuv.resize(static_cast<std::size_t>(screen_width) * screen_height * 3 / 2);
                    screen_h264.resize(screen_yuv.size());
                    std::printf("[main] screenshare on: ssrc=%u %dx%d@%d\n", screen_ssrc, screen_width, screen_height, screen_fps);

                }
                else std::printf("[main] x264 init failed — screenshare disabled\n");

            }
            else std::printf("[main] screen capture init failed — screenshare disabled\n");

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
        // LossSim's deferred sends then go out without re-stamping — over
        // the ICE lane when it won, otherwise via the relay socket.
        const auto sim_send = [&sender, &ice, &ice_active](OpenSocialNet::Network::Packet p)
        {

            if (ice_active) ice.send(OpenSocialNet::Network::packet_to_wire(p));
            else sender.send_raw(std::move(p));

        };

        // The relay rx thread and the ICE recv callback are alternative
        // sole producers for the SPSC rings — never both.
        std::thread rx { };
        if (!ice_active) rx = std::thread { receive_thread, std::ref(receiver), std::ref(incoming_queue), std::ref(*video_queue), std::ref(sender), std::ref(rtt_prober), peer_id };

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

        // Screenshare pacing, same scheme at its own (lower) rate.
        const int screen_tick_interval { std::max(1, 100 / screen_fps) };
        int ticks_since_screen { 0 };
        std::uint64_t screen_frames_sent { 0 };
        std::uint64_t screen_packets_sent { 0 };

        while (running)
        {

            while (capture.available() >= OpenSocialNet::Audio::samples_per_frame)
            {

                size_t got = capture.read(std::span<float>{chunk});
                if (got != OpenSocialNet::Audio::samples_per_frame) break;

                // Drain capture buffer even when muted so we don't backlog;
                // just skip the encode/send.
                if (audio_muted.load(std::memory_order_relaxed)) continue;

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
            if (video_send and video_active.load(std::memory_order_relaxed))
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

            // Screenshare send: tick-paced grab -> encode -> packetize,
            // same shape as the camera lane but on the H264_Screen lane.
            if (screen_send and screen_active.load(std::memory_order_relaxed) and ++ticks_since_screen >= screen_tick_interval)
            {

                ticks_since_screen = 0;
                if (screen_capture.capture_frame(std::span<std::uint8_t> { screen_yuv }) > 0)
                {

                    const int encoded { screen_encoder.encode_frame(screen_yuv.data(), screen_capture.width(), std::span<std::byte> { screen_h264 }) };
                    if (encoded > 0)
                    {

                        auto packets { screen_packetizer.packetize_frame(std::span<const std::byte> { screen_h264.data(), static_cast<std::size_t>(encoded) }) };
                        for (auto& p : packets)
                        {

                            p.header.room_id = room_id;
                            p.header.peer_id = peer_id;
                            sim.submit(std::move(p), sim_send);
                            ++screen_packets_sent;

                        }
                        ++screen_frames_sent;
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

            // RTT prober tick — emits one Ping when the interval has
            // elapsed, otherwise no-op. Same byte order swap as audio.
            // Goes around LossSim so the RTT we measure reflects the
            // actual network, not the simulator's adversarial drops.
            {

                const auto now { std::chrono::steady_clock::now() };
                if (rtt_prober.should_send_ping(now))
                {

                    auto ping { rtt_prober.build_ping(room_id, peer_id, now) };
                    if (ice_active) ice.send(OpenSocialNet::Network::packet_to_wire(ping));
                    else sender.send_raw(ping);

                }

            }

            // Keepalive when audio's gone quiet for a while. Skips the LossSim
            // path on purpose — keepalives must actually reach the relay even
            // when SIM_LOSS_PCT=100 stress-tests are running.
            if (++ticks_since_send >= keepalive_tick_interval)
            {

                OpenSocialNet::Network::Packet keepalive { };
                keepalive.header.payload_type = OpenSocialNet::Network::PayloadType::Opus;
                keepalive.header.payload_size = 0;
                if (ice_active)
                {

                    // libnice keeps the pair alive with STUN checks; this
                    // just keeps the peer's reap timers fed. Must not hit
                    // the relay socket — that would register us there.
                    sender.stamp(keepalive);
                    ice.send(OpenSocialNet::Network::packet_to_wire(keepalive));

                }
                else sender.send(keepalive);
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

                const auto agg          { mixer.stats() };
                const auto vagg         { video_router.stats() };
                const std::size_t live_peers { signaling_host.empty() ? std::size_t { 0 } : signaling.peers().size() };
                const std::string rtt_fragment { rtt_prober.format_for_stats() };
                const double elapsed_s { std::chrono::duration<double>(std::chrono::steady_clock::now() - session_steady_start).count() };

                // Human-readable .log line, same format as the old stdout
                // print. Tail this with `tail -f logs/<name>.log` during a run.
                if (stats_log_fp != nullptr)
                {

                    std::fprintf(stats_log_fp, "[net-stats] t=%.1fs src=%zu obs=%llu lost=%llu ooo=%llu jitter_ms=%.2f spsc=%zu jb=%zu jb_th=%zu jb_adapts=%llu drop_overflow=%llu sent=%d plc=%llu peers=%zu rtt=[%s]\n",
                        elapsed_s,
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
                        live_peers,
                        rtt_fragment.c_str());

                    if (vagg.sources > 0 or video_send or screen_send)
                    {

                        std::fprintf(stats_log_fp, "[vid-stats] t=%.1fs src=%zu obs=%llu lost=%llu ooo=%llu jb=%zu dec=%llu plc=%llu dropped=%llu missed=%llu rendered=%llu sent_frames=%llu sent_pkts=%llu scr_frames=%llu scr_pkts=%llu drop_overflow=%llu\n",
                            elapsed_s,
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
                            static_cast<unsigned long long>(screen_frames_sent),
                            static_cast<unsigned long long>(screen_packets_sent),
                            static_cast<unsigned long long>(video_packets_dropped_overflow.load(std::memory_order_relaxed)));

                    }

                }

                // CSV row: one per stats tick, columns match the header
                // written at startup. RTT goes into a quoted field because
                // it can contain spaces.
                if (stats_csv_fp != nullptr)
                {

                    std::fprintf(stats_csv_fp,
                        "%.2f,%zu,%llu,%llu,%llu,%.3f,%zu,%zu,%zu,%llu,%llu,%d,%llu,%zu,"
                        "%zu,%llu,%llu,%llu,%zu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                        "\"%s\"\n",
                        elapsed_s,
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
                        live_peers,
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
                        static_cast<unsigned long long>(screen_frames_sent),
                        static_cast<unsigned long long>(screen_packets_sent),
                        static_cast<unsigned long long>(video_packets_dropped_overflow.load(std::memory_order_relaxed)),
                        rtt_fragment.c_str());

                }

                ticks_since_stats = 0;

            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        }

        std::cout << "[main] shutting down, total packets sent=" << packets_sent << "\n";

        running = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        receiver.shutdown();
        if (rx.joinable()) rx.join();
        ice.shutdown();

        capture.shutdown();

        if (stats_log_fp != nullptr) { std::fclose(stats_log_fp); stats_log_fp = nullptr; }
        if (stats_csv_fp != nullptr) { std::fclose(stats_csv_fp); stats_csv_fp = nullptr; }

    } // audio_stream + mixer + sender + receiver destroyed here, before SDL_Quit

    SDL_Quit();
    return 0;

}