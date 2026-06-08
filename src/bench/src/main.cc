// related headers

// c sys headers
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// cpp stdlib headers
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// 3rd party headers

// project headers
#include "AudioConstants.hh"
#include "AudioDecoder.hh"
#include "AudioEncoder.hh"
#include "AudioPlc.hh"
#include "NetworkConstants.hh"
#include "WavIO.hh"

namespace osa    = OpenSocialNet::Audio;
namespace osnet  = OpenSocialNet::Network;
namespace osplc  = OpenSocialNet::Plc;
namespace osb    = OpenSocialNet::Bench;


namespace
{

    // CLI arg bag. Defaults pick the "minimum that runs" path: synthesize
    // 10s of audio, opus PLC, no loss, deterministic seed.
    struct Args
    {

        std::string         in_path     { "synth" };                  // path to WAV, or "synth" to synthesize
        std::string         out_path    { "bench-out.wav" };          // output WAV destination
        osplc::PlcStrategy  plc         { osplc::PlcStrategy::Opus }; // PLC strategy under test
        double              loss_pct    { 0.0 };                      // % of frames the bench will pretend never arrived
        double              duration_s  { 10.0 };                     // synthesis duration when in_path == "synth"
        std::uint64_t       seed        { 42 };                       // RNG seed for the drop coin flip — same seed → same drop pattern across strategies

    };


    osplc::PlcStrategy plc_from_str(std::string_view s) noexcept
    {

        std::string lower { s };
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "silence") return osplc::PlcStrategy::Silence;
        if (lower == "repeat" ) return osplc::PlcStrategy::Repeat;
        if (lower == "interp" ) return osplc::PlcStrategy::Interpolate;
        return osplc::PlcStrategy::Opus;

    }


    const char* plc_name(osplc::PlcStrategy s) noexcept
    {

        switch (s)
        {

            case osplc::PlcStrategy::Silence:     return "silence";
            case osplc::PlcStrategy::Repeat:      return "repeat";
            case osplc::PlcStrategy::Opus:        return "opus";
            case osplc::PlcStrategy::Interpolate: return "interp";

        }
        return "?";

    }


    // Tiny CLI parser. Format: --in PATH --out PATH --plc NAME --loss PCT
    // --duration SECS --seed N. Unknown args are ignored with a warning;
    // an arg without its value also gets ignored.
    Args parse_args(int argc, char** argv)
    {

        Args a { };

        for (int i { 1 }; i < argc; ++i)
        {

            const std::string flag { argv[i] };
            const bool needs_value { flag == "--in" or flag == "--out" or flag == "--plc" or flag == "--loss" or flag == "--duration" or flag == "--seed" };
            if (needs_value and i + 1 >= argc) { std::fprintf(stderr, "[bench] %s needs a value\n", flag.c_str()); continue; }

            if      (flag == "--in"      ) a.in_path    = argv[++i];
            else if (flag == "--out"     ) a.out_path   = argv[++i];
            else if (flag == "--plc"     ) a.plc        = plc_from_str(argv[++i]);
            else if (flag == "--loss"    ) a.loss_pct   = std::atof(argv[++i]);
            else if (flag == "--duration") a.duration_s = std::atof(argv[++i]);
            else if (flag == "--seed"    ) a.seed       = std::strtoull(argv[++i], nullptr, 10);
            else std::fprintf(stderr, "[bench] ignoring unknown arg: %s\n", flag.c_str());

        }

        return a;

    }


    // Deterministic test signal: a slow frequency sweep (200 → 800 Hz) with
    // 4 Hz amplitude modulation (vague "speech-like" syllable rate) and a
    // small noise floor. Not a substitute for real speech for serious
    // benchmarking; it's the "I don't have a WAV handy" fallback so a smoke
    // run still produces audible output.
    osb::WavData synthesize_test_signal(double duration_secs, std::uint64_t seed)
    {

        osb::WavData data { };
        data.sample_rate = osa::opus_sample_rate;
        data.channels    = 1;

        const std::size_t total_samples { static_cast<std::size_t>(duration_secs * static_cast<double>(osa::opus_sample_rate)) };
        data.samples.resize(total_samples);

        std::mt19937                       rng   { seed };
        std::normal_distribution<float>    noise { 0.0f, 0.02f };

        double phase { 0.0 };
        for (std::size_t i { 0 }; i < total_samples; ++i)
        {

            const double t       { static_cast<double>(i) / static_cast<double>(osa::opus_sample_rate) };
            const double freq_hz { 200.0 + (600.0 * t / duration_secs) };
            const double am      { 0.4 + 0.4 * std::sin(2.0 * 3.14159265358979323846 * 4.0 * t) };
            phase += 2.0 * 3.14159265358979323846 * freq_hz / static_cast<double>(osa::opus_sample_rate);
            data.samples[i] = static_cast<float>(am * std::sin(phase)) + noise(rng);

        }

        return data;

    }

}


int main(int argc, char** argv)
{

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const Args args { parse_args(argc, argv) };

    // Resolve input — either load a WAV from disk or synthesize.
    osb::WavData input { };
    if (args.in_path == "synth")
    {

        input = synthesize_test_signal(args.duration_s, args.seed);
        std::printf("[bench] synthesized %.1fs of test signal (%zu samples)\n", args.duration_s, input.samples.size());

    }
    else if (!osb::wav_read(args.in_path, input))
    {

        std::fprintf(stderr, "[bench] failed to read %s — must be 16-bit PCM mono or stereo WAV\n", args.in_path.c_str());
        return 1;

    }

    if (input.sample_rate != osa::opus_sample_rate)
    {

        std::fprintf(stderr, "[bench] input must be %u Hz; got %u Hz. Resample with `ffmpeg -i in.wav -ar %u out.wav`.\n", static_cast<unsigned>(osa::opus_sample_rate), input.sample_rate, static_cast<unsigned>(osa::opus_sample_rate));
        return 1;

    }

    // Init the audio + PLC stack. Same construction as network/main.cc, just
    // no SDL and no UDP sockets.
    osa::AudioEncoder encoder { };
    if (!encoder.valid()) { std::fprintf(stderr, "[bench] opus encoder init failed\n"); return 1; }

    osa::AudioDecoder decoder { };
    if (!decoder.valid()) { std::fprintf(stderr, "[bench] opus decoder init failed\n"); return 1; }

    osplc::AudioPlc plc { args.plc, decoder };
    // Disable the runaway-hallucination cap for bench: we want every loss
    // event to fire a real conceal call so the strategy gets exercised
    // and the RMSE / concealment count are comparable across runs. The
    // live binary keeps the default cap (5).
    plc.set_max_consecutive_concealments(1'000'000);

    // Pre-roll the drop pattern so we can peek one frame ahead (needed for
    // Interpolate's hint). Same RNG seed → same drop pattern across runs,
    // so a sweep over plc strategies sees identical loss timing.
    std::mt19937_64                          drop_rng  { args.seed };
    std::uniform_real_distribution<double>   drop_unit { 0.0, 100.0 };

    const std::size_t total_frames { input.samples.size() / osa::samples_per_frame };
    std::vector<std::uint8_t> drop_mask(total_frames, 0);
    for (std::size_t f { 0 }; f < total_frames; ++f)
    {

        drop_mask[f] = (args.loss_pct > 0.0 and drop_unit(drop_rng) < args.loss_pct) ? std::uint8_t { 1 } : std::uint8_t { 0 };

    }

    // Output accumulator. Reserve enough so we don't reallocate during the
    // hot loop; one frame in == one frame out (real decode or concealment).
    std::vector<float> output { };
    output.reserve(input.samples.size());

    // Per-frame counters for the final stats line.
    std::size_t frames_dropped { 0 };
    std::size_t frames_played  { 0 };

    // Scratch buffers reused every iteration to avoid per-frame allocation.
    std::array<float, osa::samples_per_frame>            pcm_in      { };
    std::array<float, osa::samples_per_frame>            decoded_pcm { };
    std::array<std::byte, osnet::maximum_packet_size>    encoded_buf { };

    // The bench bypasses PacketJitterBuffer on purpose: the JB's gap-skip
    // logic assumes an asynchronous receive thread filling it AHEAD of the
    // playback consumer, so a single-frame loss in a tight bench loop just
    // wedges it. The PLC integration we're benchmarking is identical to
    // network/main.cc's on_playback — `on_real_frame` on hit, `conceal` on
    // miss — and that's all we need here.
    for (std::size_t f { 0 }; f < total_frames; ++f)
    {

        const std::size_t off { f * osa::samples_per_frame };

        if (drop_mask[f] != 0)
        {

            ++frames_dropped;

            // Look one frame ahead — if the next frame isn't ALSO dropped,
            // hint its decoded samples so Interpolate has a real future
            // anchor to blend against. We use the raw input here on purpose
            // (the receiver in a real system would have to decode the next
            // buffered opus frame; for offline bench, comparing PLC output
            // against the same input is what makes RMSE meaningful).
            if (f + 1 < total_frames and drop_mask[f + 1] == 0)
            {

                std::array<float, osa::samples_per_frame> next_pcm { };
                std::copy_n(input.samples.begin() + static_cast<std::ptrdiff_t>(off + osa::samples_per_frame), osa::samples_per_frame, next_pcm.begin());
                plc.hint_next_frame(std::span<const float> { next_pcm });

            }

            const int concealed_samples { plc.conceal(std::span<float> { decoded_pcm }) };
            if (concealed_samples > 0) output.insert(output.end(), decoded_pcm.begin(), decoded_pcm.begin() + concealed_samples);
            else                       output.insert(output.end(), osa::samples_per_frame, 0.0f);
            continue;

        }

        // Real frame: encode + decode round-trip (so opus quantization error
        // is part of the output, matching what a live receiver experiences),
        // then update PLC's "last good frame" state.
        std::copy_n(input.samples.begin() + static_cast<std::ptrdiff_t>(off), osa::samples_per_frame, pcm_in.begin());

        const int encoded_bytes
        {

            encoder.encode_samples(std::span<const float> { pcm_in }, std::span<std::byte> { encoded_buf })

        };
        if (encoded_bytes <= 0) { output.insert(output.end(), osa::samples_per_frame, 0.0f); continue; }

        const int decoded_samples
        {

            decoder.decode_bytes(std::span<const std::byte> { encoded_buf.data(), static_cast<std::size_t>(encoded_bytes) }, std::span<float> { decoded_pcm })

        };
        if (decoded_samples <= 0) { output.insert(output.end(), osa::samples_per_frame, 0.0f); continue; }

        plc.on_real_frame(std::span<const float> { decoded_pcm });
        output.insert(output.end(), decoded_pcm.begin(), decoded_pcm.begin() + decoded_samples);
        ++frames_played;

    }

    const std::size_t frames_pushed { total_frames };

    osb::WavData out_wav { output, osa::opus_sample_rate, 1 };
    if (!osb::wav_write(args.out_path, out_wav))
    {

        std::fprintf(stderr, "[bench] failed to write %s\n", args.out_path.c_str());
        return 1;

    }

    // RMSE: average per-sample squared error between input and processed
    // output. Includes opus's own quantization error, so the absolute value
    // is high even at 0% loss — but the relative differences ACROSS PLC
    // strategies at the SAME loss percentage are what matter.
    double sum_sq { 0.0 };
    const std::size_t cmp_len { std::min(input.samples.size(), output.size()) };
    for (std::size_t i { 0 }; i < cmp_len; ++i)
    {

        const double diff { static_cast<double>(input.samples[i]) - static_cast<double>(output[i]) };
        sum_sq += diff * diff;

    }
    const double rmse { (cmp_len > 0) ? std::sqrt(sum_sq / static_cast<double>(cmp_len)) : 0.0 };

    // One CSV-ish line at the end so a shell loop sweeping (loss × plc)
    // produces a parseable log without any post-processing.
    std::printf("[bench] in=%s out=%s plc=%s loss=%.2f frames_pushed=%zu frames_dropped=%zu frames_played=%zu concealments=%llu rmse=%.6f\n",
        args.in_path.c_str(),
        args.out_path.c_str(),
        plc_name(args.plc),
        args.loss_pct,
        frames_pushed,
        frames_dropped,
        frames_played,
        static_cast<unsigned long long>(plc.concealments_emitted()),
        rmse);

    return 0;

}
