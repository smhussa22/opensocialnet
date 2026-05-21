// related headers
#include "AudioEncoder.hh"

// c sys headers

// cpp stdlib headers
#include <stdexcept>
// 3rd party headers
#include <opus/opus.h>

// project headers
#include "AudioConstants.hh"

namespace OpenSocialNet::Audio
{

    AudioEncoder::AudioEncoder() noexcept
    {

        int error {};
        opus_encoder.reset(opus_encoder_create(opus_sample_rate, opus_channels, OPUS_APPLICATION_VOIP, &error));
        if (error != OPUS_OK) opus_encoder.reset();

    }

    int AudioEncoder::encode_samples(std::span<const float> input_pcm_samples, std::span<std::byte> output_encoded_bytes) noexcept
    {

        if (!valid()) return -1;
        if (input_pcm_samples.size() != samples_per_frame) return -1;
        const int encoded_bytes_written { opus_encode_float(opus_encoder.get(), input_pcm_samples.data(), static_cast<int>(input_pcm_samples.size()),
            reinterpret_cast<unsigned char*>(output_encoded_bytes.data()), static_cast<opus_int32>(output_encoded_bytes.size())) };

        return encoded_bytes_written;

    }

    void AudioEncoder::reset() noexcept
    {

        if (!valid()) return;
        opus_encoder_ctl(opus_encoder.get(), OPUS_RESET_STATE);

    }

    bool AudioEncoder::valid() const noexcept
    {

        return (opus_encoder != nullptr);

    }

}
