#ifndef AUDIO_CONSTANTS_HH
#define AUDIO_CONSTANTS_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers

namespace OpenSocialNet::Audio
{

    inline constexpr std::int32_t opus_sample_rate { 48000 };
    inline constexpr std::int32_t opus_channels    { 1 };
    inline constexpr std::int16_t samples_per_frame         { 480 };

    inline SDL_AudioSpec create_opus_audio_spec()
    {

        SDL_AudioSpec spec {};

        spec.channels = opus_channels;
        spec.format   = SDL_AUDIO_F32;
        spec.freq     = opus_sample_rate;

        return spec;

    }


}

#endif // AUDIO_CONSTANTS_HH