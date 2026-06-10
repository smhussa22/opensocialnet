// related headers

// c sys headers
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// cpp stdlib headers
#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "AudioCapture.hh"
#include "AudioConstants.hh"


namespace
{

    // Minimal RIFF/WAVE writer — 16-bit PCM, mono, opus sample rate. The
    // bench's own WavIO would work too but lives in src/bench/, which the
    // network module shouldn't pull on. ~30 lines inlined here is cheaper
    // than restructuring for that.
    bool write_wav_s16_mono(const std::string& path, const std::vector<float>& samples, std::uint32_t sample_rate)
    {

        std::FILE* f { std::fopen(path.c_str(), "wb") };
        if (f == nullptr) return false;

        const std::uint32_t num_samples  { static_cast<std::uint32_t>(samples.size()) };
        const std::uint32_t data_bytes   { num_samples * 2 };               // s16 = 2 bytes/sample
        const std::uint32_t riff_size    { 36 + data_bytes };

        auto put_u32 { [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); } };
        auto put_u16 { [&](std::uint16_t v) { std::fwrite(&v, 2, 1, f); } };

        std::fwrite("RIFF", 1, 4, f);
        put_u32(riff_size);
        std::fwrite("WAVEfmt ", 1, 8, f);
        put_u32(16);                                                        // PCM fmt chunk size
        put_u16(1);                                                         // format = PCM
        put_u16(1);                                                         // channels = mono
        put_u32(sample_rate);
        put_u32(sample_rate * 2);                                           // byte rate
        put_u16(2);                                                         // block align
        put_u16(16);                                                        // bits/sample
        std::fwrite("data", 1, 4, f);
        put_u32(data_bytes);

        for (float s : samples)
        {

            const float clamped { std::clamp(s, -1.0f, 1.0f) };
            const std::int16_t v { static_cast<std::int16_t>(clamped * 32767.0f) };
            std::fwrite(&v, 2, 1, f);

        }

        std::fclose(f);
        return true;

    }

    // Same case-insensitive substring picker as network/main.cc, copy/paste
    // because main.cc keeps it `static` and the two binaries don't currently
    // share helpers. If a third caller appears, lift it into a helper.hh.
    SDL_AudioDeviceID pick_input(std::string_view wanted)
    {

        int                count { 0 };
        SDL_AudioDeviceID* ids   { ::SDL_GetAudioRecordingDevices(&count) };
        if (ids == nullptr or count <= 0)
        {

            std::printf("[record] no input devices reported by SDL — falling back to default\n");
            if (ids) ::SDL_free(ids);
            return SDL_AUDIO_DEVICE_DEFAULT_RECORDING;

        }

        std::printf("[record] input devices (%d):\n", count);
        SDL_AudioDeviceID chosen { 0 };
        for (int i { 0 }; i < count; ++i)
        {

            const char* name { ::SDL_GetAudioDeviceName(ids[i]) };
            const std::string nm { name ? name : "(unnamed)" };
            std::printf("  [%d] id=%u %s\n", i, static_cast<unsigned>(ids[i]), nm.c_str());

            if (chosen != 0 or wanted.empty()) continue;
            std::string lhs { nm }, rhs { wanted };
            for (auto& c : lhs) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (auto& c : rhs) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lhs.find(rhs) != std::string::npos) chosen = ids[i];

        }
        ::SDL_free(ids);
        return chosen != 0 ? chosen : SDL_AUDIO_DEVICE_DEFAULT_RECORDING;

    }

}


int main(int argc, char** argv)
{

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    double      duration_s { 10.0 };
    std::string out_path   { "/tmp/recorded_voice.wav" };

    for (int i { 1 }; i + 1 < argc; i += 2)
    {

        const std::string flag { argv[i] };
        if      (flag == "--duration") duration_s = std::atof(argv[i + 1]);
        else if (flag == "--out"     ) out_path   = argv[i + 1];

    }

    if (!::SDL_Init(SDL_INIT_AUDIO))
    {

        std::fprintf(stderr, "[record] SDL_Init: %s\n", ::SDL_GetError());
        return 1;

    }

    const char* env_input { std::getenv("OSN_AUDIO_INPUT") };
    const SDL_AudioDeviceID dev { pick_input(env_input ? env_input : "") };
    std::printf("[record] using device id=%u, duration=%.1fs, out=%s\n", static_cast<unsigned>(dev), duration_s, out_path.c_str());

    OpenSocialNet::Audio::AudioCapture capture { };
    if (!capture.init(dev))
    {

        std::fprintf(stderr, "[record] capture.init() failed: %s\n", ::SDL_GetError());
        return 1;

    }

    const std::size_t target_samples { static_cast<std::size_t>(duration_s * static_cast<double>(OpenSocialNet::Audio::opus_sample_rate)) };
    std::vector<float> all { };
    all.reserve(target_samples);

    std::vector<float> tmp(OpenSocialNet::Audio::samples_per_frame);

    const auto start { std::chrono::steady_clock::now() };
    while (all.size() < target_samples)
    {

        while (capture.available() >= OpenSocialNet::Audio::samples_per_frame and all.size() < target_samples)
        {

            const std::size_t got { capture.read(std::span<float> { tmp }) };
            if (got == 0) break;
            all.insert(all.end(), tmp.begin(), tmp.begin() + got);

        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Safety bail if SDL never feeds us anything (broken device, silent
        // ALSA, etc) — better to fail fast than loop forever.
        const auto elapsed { std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() };
        if (elapsed > duration_s + 2.0 and all.empty())
        {

            std::fprintf(stderr, "[record] no samples after %.1fs — check OSN_AUDIO_INPUT / device perms\n", elapsed);
            return 1;

        }

    }

    if (!write_wav_s16_mono(out_path, all, OpenSocialNet::Audio::opus_sample_rate))
    {

        std::fprintf(stderr, "[record] failed to write %s\n", out_path.c_str());
        return 1;

    }

    std::printf("[record] wrote %zu samples (%.1f s) to %s\n", all.size(), static_cast<double>(all.size()) / static_cast<double>(OpenSocialNet::Audio::opus_sample_rate), out_path.c_str());
    return 0;

}
