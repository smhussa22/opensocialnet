#ifndef AUDIO_DECODER_HH
#define AUDIO_DECODER_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <span>
#include <cstddef>

// 3rd party headers
#include <opus/opus.h>

// project headers
#include "AudioDecoderDeleter.hh"

namespace OpenSocialNet::Audio
{

    class AudioDecoder
    {

    public:
        explicit AudioDecoder() noexcept;
        ~AudioDecoder() = default;

        AudioDecoder(const AudioDecoder&) = delete;
        AudioDecoder& operator=(const AudioDecoder&) = delete;
        AudioDecoder(AudioDecoder&&) = default;
        AudioDecoder& operator=(AudioDecoder&&) = default;

        // decodes Opus encoded bytes into PCM float samples
        int decode_bytes (std::span<const std::byte> encoded_data, std::span<float> output_pcm_bytes) noexcept;

        // generates concealed PCM samples for a missing/lost packet.
        int decode_missing_bytes (std::span<float> output_pcm_bytes) noexcept;

        // resets the internal decoder state/history.
        void reset() noexcept;

        // returns whether the decoder was successfully initialized.
        [[nodiscard]] bool valid() const noexcept;

    private:
        OpusDecoderPtr opus_decoder {}; // owns the underlying native Opus decoder resource.

    };

};

#endif // AUDIO_DECODER_HH