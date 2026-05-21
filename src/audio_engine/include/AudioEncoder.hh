#ifndef AUDIO_ENCODER_HH
#define AUDIO_ENCODER_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <span>
#include <cstddef>

// 3rd party headers
#include <opus/opus.h>

// project headers
#include "AudioEncoderDeleter.hh"

namespace OpenSocialNet::Audio
{

    class AudioEncoder
    {

    public:
        explicit AudioEncoder() noexcept;
        ~AudioEncoder() = default;

        AudioEncoder(const AudioEncoder&) = delete;
        AudioEncoder& operator=(const AudioEncoder&) = delete;
        AudioEncoder(AudioEncoder&&) = default;
        AudioEncoder& operator=(AudioEncoder&&) = default;

        // encodes PCM float samples into an Opus encoded byte payload. returns -1 on error.
        int encode_samples (std::span<const float> input_pcm_samples, std::span<std::byte> output_encoded_bytes) noexcept;

        // resets the internal encoder state/history.
        void reset() noexcept;

        // returns whether the encoder was successfully initialized.
        [[nodiscard]] bool valid() const noexcept;

    private:
        OpusEncoderPtr opus_encoder {}; // owns the underlying native Opus encoder resource.

    };

};

#endif // AUDIO_ENCODER_HH
