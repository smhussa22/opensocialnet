// related headers
#include "AudioDecoder.hh"

// c sys headers

// cpp stdlib headers
#include <stdexcept>
// 3rd party headers
#include <opus/opus.h>

// project headers
#include "AudioConstants.hh"

namespace OpenSocialNet::Audio
{

    AudioDecoder::AudioDecoder() noexcept
    {

        int error {};
        opus_decoder.reset(opus_decoder_create(opus_sample_rate, opus_channels, &error));
        if (error != OPUS_OK) opus_decoder.reset();

    }

    int AudioDecoder::decode_bytes(std::span<const std::byte> encoded_data, std::span<float> output_pcm_bytes) noexcept
    {

        if (!valid()) return -1;
        if (output_pcm_bytes.size() != samples_per_frame) return -1;
        const int decoded_samples_written { opus_decode_float(opus_decoder.get(), reinterpret_cast<const unsigned char*>(encoded_data.data()), static_cast<opus_int32>(encoded_data.size()),
            output_pcm_bytes.data(), static_cast<int>(output_pcm_bytes.size()), 0) };

        return decoded_samples_written;

    }

    int AudioDecoder::decode_missing_bytes(std::span<float> output_pcm_bytes) noexcept
    {

        if (!valid()) return -1;
        if (output_pcm_bytes.size() != samples_per_frame) return -1;
        const int decoded_samples_written { opus_decode_float(opus_decoder.get(), nullptr, 0,
            output_pcm_bytes.data(), static_cast<int>(output_pcm_bytes.size()), 0) };

        return decoded_samples_written;

    }

    void AudioDecoder::reset() noexcept
    {

        if (!valid()) return;
        opus_decoder_ctl(opus_decoder.get(), OPUS_RESET_STATE);

    }

    bool AudioDecoder::valid() const noexcept
    {

        return (opus_decoder != nullptr);

    }

}
