#ifndef AUDIO_CONSTANTS_HH
#define AUDIO_CONSTANTS_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers

// 3rd party headers

// project headers

// Pure-data constants; no SDL or opus dependency on purpose so non-audio
// callers (the bench harness, the relay, anyone post-SDL refactor) can
// pull in just the numbers without dragging the windowing stack.
// The SDL_AudioSpec helper that used to live here moved to AudioStream.hh
// since it materialises an SDL type.
namespace OpenSocialNet::Audio
{

    inline constexpr std::int32_t opus_sample_rate  { 48000 };
    inline constexpr std::int32_t opus_channels     { 1 };
    inline constexpr std::int16_t samples_per_frame { 480 };

}

#endif // AUDIO_CONSTANTS_HH