#ifndef OSN_BENCH_WAV_IO_HH
#define OSN_BENCH_WAV_IO_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <filesystem>
#include <vector>

// 3rd party headers

// project headers

namespace OpenSocialNet::Bench
{

    // Single mono channel of float audio normalized to [-1.0, 1.0]. Stereo
    // sources get downmixed to mono on read so the rest of the bench can
    // assume 1-channel input. Sample rate is preserved from the file; the
    // caller (main.cc) checks that it matches opus's expected rate before
    // feeding the encoder.
    struct WavData
    {

        std::vector<float> samples       { };      // PCM, [-1.0, 1.0]
        std::uint32_t      sample_rate   { 48000 }; // Hz
        std::uint16_t      channels      { 1 };    // always 1 after wav_read; wav_write only emits mono

    };


    // Parses RIFF/WAVE: 16-bit PCM only, mono or stereo (stereo → averaged
    // to mono). Returns false on any header mismatch, unknown subchunk
    // order, unsupported bit-depth/format, or short read. Doesn't attempt
    // to be a general WAV reader — just enough to round-trip the formats
    // Audacity / ffmpeg produce when you "Export as 16-bit signed PCM
    // mono 48 kHz".
    bool wav_read(const std::filesystem::path& path, WavData& out);


    // Writes 16-bit signed PCM mono. Float samples are clamped to
    // [-1.0, 1.0] and scaled to int16 range before write. Returns false
    // on any I/O failure.
    bool wav_write(const std::filesystem::path& path, const WavData& data);

}

#endif // OSN_BENCH_WAV_IO_HH
