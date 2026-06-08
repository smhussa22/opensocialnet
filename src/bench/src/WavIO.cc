// related headers
#include "WavIO.hh"

// c sys headers
#include <cstdint>
#include <cstring>

// cpp stdlib headers
#include <algorithm>
#include <cstddef>
#include <fstream>

// 3rd party headers

// project headers

namespace OpenSocialNet::Bench
{

    namespace
    {

        // Canonical 44-byte RIFF/WAVE PCM header. Bench-grade — doesn't
        // parse extension chunks, list chunks, anything fancy. If a file
        // has those, write a stripped copy with ffmpeg first.
        #pragma pack(push, 1)
        struct WavHeader
        {

            char          riff_id[4];          // "RIFF"
            std::uint32_t riff_size;           // file_size - 8
            char          wave_id[4];          // "WAVE"
            char          fmt_id[4];           // "fmt "
            std::uint32_t fmt_size;            // 16 for canonical PCM
            std::uint16_t fmt_format;          // 1 = PCM
            std::uint16_t channels;
            std::uint32_t sample_rate;
            std::uint32_t byte_rate;
            std::uint16_t block_align;
            std::uint16_t bits_per_sample;
            char          data_id[4];          // "data"
            std::uint32_t data_size;

        };
        #pragma pack(pop)

        static_assert(sizeof(WavHeader) == 44, "WavHeader must be exactly 44 bytes");

    }


    bool wav_read(const std::filesystem::path& path, WavData& out)
    {

        std::ifstream f { path, std::ios::binary };
        if (!f) return false;

        WavHeader h { };
        f.read(reinterpret_cast<char*>(&h), sizeof(h));
        if (f.gcount() != static_cast<std::streamsize>(sizeof(h))) return false;
        if (std::memcmp(h.riff_id, "RIFF", 4) != 0) return false;
        if (std::memcmp(h.wave_id, "WAVE", 4) != 0) return false;
        if (std::memcmp(h.fmt_id,  "fmt ", 4) != 0) return false;
        if (std::memcmp(h.data_id, "data", 4) != 0) return false;
        if (h.fmt_format     != 1)  return false; // PCM only
        if (h.bits_per_sample != 16) return false; // 16-bit only
        if (h.channels != 1 and h.channels != 2) return false;

        out.sample_rate = h.sample_rate;

        const std::size_t total_int16_samples { h.data_size / sizeof(std::int16_t) };
        std::vector<std::int16_t> raw(total_int16_samples);
        f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
        if (f.gcount() != static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t))) return false;

        if (h.channels == 1)
        {

            out.samples.resize(total_int16_samples);
            for (std::size_t i { 0 }; i < total_int16_samples; ++i) out.samples[i] = static_cast<float>(raw[i]) / 32768.0f;

        }
        else // 2 channels, average to mono
        {

            const std::size_t mono_samples { total_int16_samples / 2 };
            out.samples.resize(mono_samples);
            for (std::size_t i { 0 }; i < mono_samples; ++i)
            {

                const float l { static_cast<float>(raw[i * 2    ]) / 32768.0f };
                const float r { static_cast<float>(raw[i * 2 + 1]) / 32768.0f };
                out.samples[i] = (l + r) * 0.5f;

            }

        }
        out.channels = 1;

        return true;

    }


    bool wav_write(const std::filesystem::path& path, const WavData& data)
    {

        std::ofstream f { path, std::ios::binary };
        if (!f) return false;

        WavHeader h { };
        std::memcpy(h.riff_id, "RIFF", 4);
        std::memcpy(h.wave_id, "WAVE", 4);
        std::memcpy(h.fmt_id,  "fmt ", 4);
        std::memcpy(h.data_id, "data", 4);
        h.fmt_size        = 16;
        h.fmt_format      = 1;
        h.channels        = 1;
        h.sample_rate     = data.sample_rate;
        h.bits_per_sample = 16;
        h.byte_rate       = h.sample_rate * h.channels * (h.bits_per_sample / 8);
        h.block_align     = h.channels * (h.bits_per_sample / 8);
        h.data_size       = static_cast<std::uint32_t>(data.samples.size() * sizeof(std::int16_t));
        h.riff_size       = h.data_size + sizeof(h) - 8;

        f.write(reinterpret_cast<const char*>(&h), sizeof(h));

        // Float [-1.0, 1.0] → int16 with clamping.
        std::vector<std::int16_t> raw(data.samples.size());
        for (std::size_t i { 0 }; i < data.samples.size(); ++i)
        {

            const float clamped { std::min(1.0f, std::max(-1.0f, data.samples[i])) };
            raw[i] = static_cast<std::int16_t>(clamped * 32767.0f);

        }
        f.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));

        return f.good();

    }

}
