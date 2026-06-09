// related headers

// c sys headers
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// cpp stdlib headers
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// 3rd party headers

// project headers
#include "AudioConstants.hh"
#include "AudioDecoder.hh"
#include "AudioEncoder.hh"
#include "AudioPlc.hh"
#include "NetworkConstants.hh"
#include "Packet.hh"
#include "PacketJitterBuffer.hh"
#include "WavIO.hh"

namespace osa    = OpenSocialNet::Audio;
namespace osnet  = OpenSocialNet::Network;
namespace osplc  = OpenSocialNet::Plc;
namespace osb    = OpenSocialNet::Bench;


namespace
{

    // Which receive path the bench drives. Bypass is the original
    // PLC-direct path (no JB, no timing, no JitterStats) — kept so the
    // PLC sweep numbers stay reproducible. Fixed + Adaptive both wire
    // the real PacketJitterBuffer in front of the decoder/PLC; they
    // differ only in whether set_adaptive() is on.
    enum class JbMode { Bypass, Fixed, Adaptive };


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
        JbMode              jb_mode     { JbMode::Bypass };           // bypass | fixed | adaptive
        double              jitter_ms   { 0.0 };                      // max one-way arrival jitter, uniform [0, jitter_ms]; 0 = perfectly spaced arrivals
        double              startup_ms  { 30.0 };                     // delay before the first pop so the JB has time to fill past playout_threshold

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


    JbMode jb_mode_from_str(std::string_view s) noexcept
    {

        std::string lower { s };
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "fixed"   ) return JbMode::Fixed;
        if (lower == "adaptive") return JbMode::Adaptive;
        return JbMode::Bypass;

    }


    const char* jb_mode_name(JbMode m) noexcept
    {

        switch (m)
        {

            case JbMode::Bypass:   return "bypass";
            case JbMode::Fixed:    return "fixed";
            case JbMode::Adaptive: return "adaptive";

        }
        return "?";

    }


    // Tiny CLI parser. Format: --in PATH --out PATH --plc NAME --loss PCT
    // --duration SECS --seed N --jb-mode NAME --jitter-ms N --startup-ms N.
    // Unknown args are ignored with a warning; arg without value also ignored.
    Args parse_args(int argc, char** argv)
    {

        Args a { };

        for (int i { 1 }; i < argc; ++i)
        {

            const std::string flag { argv[i] };
            const bool needs_value
            {

                flag == "--in" or flag == "--out" or flag == "--plc" or flag == "--loss"
                or flag == "--duration" or flag == "--seed" or flag == "--jb-mode"
                or flag == "--jitter-ms" or flag == "--startup-ms"

            };
            if (needs_value and i + 1 >= argc) { std::fprintf(stderr, "[bench] %s needs a value\n", flag.c_str()); continue; }

            if      (flag == "--in"        ) a.in_path    = argv[++i];
            else if (flag == "--out"       ) a.out_path   = argv[++i];
            else if (flag == "--plc"       ) a.plc        = plc_from_str(argv[++i]);
            else if (flag == "--loss"      ) a.loss_pct   = std::atof(argv[++i]);
            else if (flag == "--duration"  ) a.duration_s = std::atof(argv[++i]);
            else if (flag == "--seed"      ) a.seed       = std::strtoull(argv[++i], nullptr, 10);
            else if (flag == "--jb-mode"   ) a.jb_mode    = jb_mode_from_str(argv[++i]);
            else if (flag == "--jitter-ms" ) a.jitter_ms  = std::atof(argv[++i]);
            else if (flag == "--startup-ms") a.startup_ms = std::atof(argv[++i]);
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


    // Stats returned from either receive path so main() can print a
    // single CSV-ish summary line regardless of which path ran.
    struct RunStats
    {

        std::size_t frames_pushed     { 0 }; // packets the sender attempted to transmit (= total_frames)
        std::size_t frames_dropped    { 0 }; // packets the sender pretended never arrived (loss simulation)
        std::size_t frames_played     { 0 }; // real frames the receiver decoded + emitted
        std::size_t pops_attempted    { 0 }; // total pop ticks the receiver ran (JB modes only; equals total_frames)
        std::size_t pops_underrun     { 0 }; // pops that returned no packet → PLC concealment fired
        std::size_t final_threshold   { 0 }; // playout threshold at end of run (JB modes only)
        std::size_t max_threshold     { 0 }; // peak playout threshold during run
        double      mean_depth_frames { 0.0 }; // time-averaged buffer depth in frames; *10ms ≈ added latency in ms
        std::size_t adaptations       { 0 }; // jitter_buffer.adaptation_count() at end
        double      final_jitter_ms   { 0.0 }; // smoothed RFC 3550 jitter measured by JitterStats at end

    };


    // Bypass path: original tight-loop PLC bench. No JB, no wall-clock
    // sleeping. Used for the PLC-strategy A/B sweep.
    RunStats run_bypass_path(const Args& /*args*/, const osb::WavData& input, osa::AudioEncoder& encoder, osa::AudioDecoder& decoder, osplc::AudioPlc& plc, const std::vector<std::uint8_t>& drop_mask, std::vector<float>& output)
    {

        RunStats stats { };
        const std::size_t total_frames { drop_mask.size() };
        stats.frames_pushed = total_frames;

        std::array<float,        osa::samples_per_frame>           pcm_in      { };
        std::array<float,        osa::samples_per_frame>           decoded_pcm { };
        std::array<std::byte,    osnet::maximum_packet_size>       encoded_buf { };

        for (std::size_t f { 0 }; f < total_frames; ++f)
        {

            const std::size_t off { f * osa::samples_per_frame };

            if (drop_mask[f] != 0)
            {

                ++stats.frames_dropped;

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

            std::copy_n(input.samples.begin() + static_cast<std::ptrdiff_t>(off), osa::samples_per_frame, pcm_in.begin());

            const int encoded_bytes { encoder.encode_samples(std::span<const float> { pcm_in }, std::span<std::byte> { encoded_buf }) };
            if (encoded_bytes <= 0) { output.insert(output.end(), osa::samples_per_frame, 0.0f); continue; }

            const int decoded_samples { decoder.decode_bytes(std::span<const std::byte> { encoded_buf.data(), static_cast<std::size_t>(encoded_bytes) }, std::span<float> { decoded_pcm }) };
            if (decoded_samples <= 0) { output.insert(output.end(), osa::samples_per_frame, 0.0f); continue; }

            plc.on_real_frame(std::span<const float> { decoded_pcm });
            output.insert(output.end(), decoded_pcm.begin(), decoded_pcm.begin() + decoded_samples);
            ++stats.frames_played;

        }

        return stats;

    }


    // JB path: pre-encode every non-dropped frame, schedule per-frame
    // arrival times with uniform jitter, then drive a real time-domain
    // event loop that interleaves Packet pushes (sender) and pops
    // (receiver every 10 ms). This is the path that exercises the
    // adaptive playout code we're benchmarking.
    RunStats run_jb_path(const Args& args, const osb::WavData& input, osa::AudioEncoder& encoder, osa::AudioDecoder& decoder, osplc::AudioPlc& plc, const std::vector<std::uint8_t>& drop_mask, std::vector<float>& output)
    {

        using steady_clock = std::chrono::steady_clock;

        RunStats stats { };
        const std::size_t total_frames { drop_mask.size() };
        stats.frames_pushed = total_frames;

        // Configure the buffer. Adaptive mode opts in; otherwise the
        // threshold stays where the constructor put it (2 frames = 20 ms).
        osnet::PacketJitterBuffer jb { };
        if (args.jb_mode == JbMode::Adaptive) jb.set_adaptive(true);

        // Encoded payloads for non-dropped frames. We pre-encode rather
        // than encoding inside the hot loop because the encoder is the
        // single slowest thing per frame and we don't want its cost to
        // contaminate the arrival timeline.
        struct PreEncoded { std::size_t frame_idx; std::vector<std::byte> bytes; };
        std::vector<PreEncoded> encoded_frames { };
        encoded_frames.reserve(total_frames);

        std::array<float,        osa::samples_per_frame>     pcm_in      { };
        std::array<std::byte,    osnet::maximum_packet_size> encoded_buf { };
        for (std::size_t f { 0 }; f < total_frames; ++f)
        {

            if (drop_mask[f] != 0) { ++stats.frames_dropped; continue; }

            const std::size_t off { f * osa::samples_per_frame };
            std::copy_n(input.samples.begin() + static_cast<std::ptrdiff_t>(off), osa::samples_per_frame, pcm_in.begin());
            const int n { encoder.encode_samples(std::span<const float> { pcm_in }, std::span<std::byte> { encoded_buf }) };
            if (n <= 0) continue;

            PreEncoded pe { };
            pe.frame_idx = f;
            pe.bytes.assign(encoded_buf.begin(), encoded_buf.begin() + n);
            encoded_frames.push_back(std::move(pe));

        }

        // Build the arrival timeline. Send time for frame f is
        // f * 10 ms; we add uniform [0, jitter_ms] of arrival jitter and
        // re-sort so out-of-order arrivals are physically possible (the
        // PacketJitterBuffer is what's supposed to reorder them).
        struct Arrival { std::int64_t time_us; std::size_t frame_idx; const std::vector<std::byte>* bytes; };
        std::vector<Arrival> arrivals { };
        arrivals.reserve(encoded_frames.size());

        std::mt19937_64                       jitter_rng  { args.seed ^ 0x9E3779B97F4A7C15ull }; // distinct stream from drop_rng so loss + jitter don't correlate
        std::uniform_real_distribution<double> jitter_unit { 0.0, args.jitter_ms * 1000.0 };

        constexpr std::int64_t frame_interval_us { 10000 };
        for (auto& pe : encoded_frames)
        {

            const std::int64_t send_us   { static_cast<std::int64_t>(pe.frame_idx) * frame_interval_us };
            const std::int64_t jitter_us { args.jitter_ms > 0.0 ? static_cast<std::int64_t>(jitter_unit(jitter_rng)) : 0 };
            arrivals.push_back({ send_us + jitter_us, pe.frame_idx, &pe.bytes });

        }
        std::sort(arrivals.begin(), arrivals.end(), [](const Arrival& a, const Arrival& b) { return a.time_us < b.time_us; });

        // Pop schedule: one tick every 10 ms starting startup_ms after t0.
        const std::int64_t startup_us           { static_cast<std::int64_t>(args.startup_ms * 1000.0) };
        const std::size_t  total_pops           { total_frames };
        const auto         t0                   { steady_clock::now() };
        std::size_t        arrival_idx          { 0 };
        std::size_t        pop_idx              { 0 };

        // For mean-depth: sample buffer depth at every pop tick and
        // average at the end. depth here = jb.size() (queued packets);
        // the "playout threshold" is mean_threshold.
        std::size_t depth_sum_frames { 0 };
        std::size_t depth_samples    { 0 };
        std::size_t threshold_sum    { 0 };
        std::size_t threshold_samples { 0 };

        std::array<float, osa::samples_per_frame> decoded_pcm { };

        // Event loop. At each step decide whether the next arrival or
        // the next pop comes first and sleep until that instant. The
        // bench takes roughly real-time to run (10s of audio ≈ 10s
        // wall clock); fine for a graph-grade benchmark.
        while (pop_idx < total_pops or arrival_idx < arrivals.size())
        {

            const std::int64_t next_arrival_us { (arrival_idx < arrivals.size()) ? arrivals[arrival_idx].time_us
                                                                                 : std::numeric_limits<std::int64_t>::max() };
            const std::int64_t next_pop_us     { (pop_idx     < total_pops    )  ? startup_us + static_cast<std::int64_t>(pop_idx) * frame_interval_us
                                                                                 : std::numeric_limits<std::int64_t>::max() };

            const bool arrival_first { next_arrival_us <= next_pop_us };
            const std::int64_t target_us { arrival_first ? next_arrival_us : next_pop_us };
            std::this_thread::sleep_until(t0 + std::chrono::microseconds { target_us });

            if (arrival_first)
            {

                const Arrival& a { arrivals[arrival_idx] };

                osnet::Packet pkt { };
                pkt.header.payload_type = osnet::PayloadType::Opus;
                pkt.header.sequence     = static_cast<std::uint16_t>(a.frame_idx);
                // ts in "microseconds-equivalent ticks" so JitterStats'
                // D(i,j) = arrival_delta - ts_delta cancels cleanly at
                // zero jitter and shows real jitter otherwise.
                pkt.header.timestamp    = static_cast<std::uint32_t>(static_cast<std::int64_t>(a.frame_idx) * frame_interval_us);
                pkt.header.payload_size = static_cast<std::uint16_t>(a.bytes->size());
                std::memcpy(pkt.payload, a.bytes->data(), a.bytes->size());

                jb.push(pkt);
                ++arrival_idx;
                continue;

            }

            // Pop tick.
            ++pop_idx;
            ++stats.pops_attempted;
            depth_sum_frames += jb.size();
            ++depth_samples;
            threshold_sum    += jb.current_playout_threshold();
            ++threshold_samples;
            stats.max_threshold = std::max(stats.max_threshold, jb.current_playout_threshold());

            osnet::Packet popped { };
            if (jb.pop(popped))
            {

                const int n { decoder.decode_bytes(std::span<const std::byte> { reinterpret_cast<const std::byte*>(popped.payload), popped.header.payload_size }, std::span<float> { decoded_pcm }) };
                if (n > 0)
                {

                    plc.on_real_frame(std::span<const float> { decoded_pcm });
                    output.insert(output.end(), decoded_pcm.begin(), decoded_pcm.begin() + n);
                    ++stats.frames_played;

                }
                else
                {

                    output.insert(output.end(), osa::samples_per_frame, 0.0f);

                }

            }
            else
            {

                ++stats.pops_underrun;
                const int n { plc.conceal(std::span<float> { decoded_pcm }) };
                if (n > 0) output.insert(output.end(), decoded_pcm.begin(), decoded_pcm.begin() + n);
                else       output.insert(output.end(), osa::samples_per_frame, 0.0f);

            }

        }

        stats.final_threshold = jb.current_playout_threshold();
        stats.adaptations     = jb.adaptation_count();
        stats.final_jitter_ms = jb.stats().jitter_ms();
        if (depth_samples > 0)     stats.mean_depth_frames = static_cast<double>(depth_sum_frames) / static_cast<double>(depth_samples);
        return stats;

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

    // Build the drop mask once. Same RNG seed → same drop pattern across
    // jb-modes, so a fixed-vs-adaptive A/B sees identical loss timing.
    std::mt19937_64                          drop_rng  { args.seed };
    std::uniform_real_distribution<double>   drop_unit { 0.0, 100.0 };

    const std::size_t total_frames { input.samples.size() / osa::samples_per_frame };
    std::vector<std::uint8_t> drop_mask(total_frames, 0);
    for (std::size_t f { 0 }; f < total_frames; ++f)
    {

        drop_mask[f] = (args.loss_pct > 0.0 and drop_unit(drop_rng) < args.loss_pct) ? std::uint8_t { 1 } : std::uint8_t { 0 };

    }

    std::vector<float> output { };
    output.reserve(input.samples.size());

    std::printf("[bench] mode=%s plc=%s loss=%.2f%% jitter_ms=%.1f frames=%zu\n", jb_mode_name(args.jb_mode), plc_name(args.plc), args.loss_pct, args.jitter_ms, total_frames);

    const RunStats stats { (args.jb_mode == JbMode::Bypass)
                          ? run_bypass_path(args, input, encoder, decoder, plc, drop_mask, output)
                          : run_jb_path   (args, input, encoder, decoder, plc, drop_mask, output) };

    osb::WavData out_wav { output, osa::opus_sample_rate, 1 };
    if (!osb::wav_write(args.out_path, out_wav))
    {

        std::fprintf(stderr, "[bench] failed to write %s\n", args.out_path.c_str());
        return 1;

    }

    // RMSE: average per-sample squared error between input and processed
    // output. Includes opus's own quantization error so the absolute
    // value is high even at 0% loss; relative differences ACROSS PLC
    // strategies / JB modes at the SAME loss percentage are what matter.
    double sum_sq { 0.0 };
    const std::size_t cmp_len { std::min(input.samples.size(), output.size()) };
    for (std::size_t i { 0 }; i < cmp_len; ++i)
    {

        const double diff { static_cast<double>(input.samples[i]) - static_cast<double>(output[i]) };
        sum_sq += diff * diff;

    }
    const double rmse { (cmp_len > 0) ? std::sqrt(sum_sq / static_cast<double>(cmp_len)) : 0.0 };

    // One CSV-ish line at the end so a shell loop sweeping
    // (loss × plc × jb-mode) produces a parseable log.
    std::printf("[bench] in=%s out=%s plc=%s mode=%s loss=%.2f jitter_ms=%.1f frames_pushed=%zu frames_dropped=%zu frames_played=%zu pops_underrun=%zu jb_final_th=%zu jb_max_th=%zu jb_mean_depth=%.2f jb_adapts=%zu jb_final_jitter_ms=%.2f concealments=%llu rmse=%.6f\n",
        args.in_path.c_str(),
        args.out_path.c_str(),
        plc_name(args.plc),
        jb_mode_name(args.jb_mode),
        args.loss_pct,
        args.jitter_ms,
        stats.frames_pushed,
        stats.frames_dropped,
        stats.frames_played,
        stats.pops_underrun,
        stats.final_threshold,
        stats.max_threshold,
        stats.mean_depth_frames,
        stats.adaptations,
        stats.final_jitter_ms,
        static_cast<unsigned long long>(plc.concealments_emitted()),
        rmse);

    return 0;

}
