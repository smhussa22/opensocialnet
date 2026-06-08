#ifndef SDL_AUDIO_STREAM_HH
#define SDL_AUDIO_STREAM_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "AudioConstants.hh"
#include "AudioStreamDeleter.hh"

namespace OpenSocialNet::Audio
{

    // Build an SDL_AudioSpec configured for the opus pipeline's PCM format
    // (mono float32 at opus_sample_rate). Lives here rather than in
    // AudioConstants.hh so non-SDL consumers (the bench harness, the relay)
    // don't have to transitively pull SDL into their build.
    inline SDL_AudioSpec create_opus_audio_spec() noexcept
    {

        SDL_AudioSpec spec { };
        spec.channels = opus_channels;
        spec.format   = SDL_AUDIO_F32;
        spec.freq     = opus_sample_rate;
        return spec;

    }


    class AudioStream
    {
    public:
        explicit AudioStream(const SDL_AudioSpec& spec, SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, SDL_AudioStreamCallback callback = nullptr, void* userdata = nullptr);
        void resume() noexcept;
        void pause() noexcept;
        void put_audio_data(const void* data, int size) noexcept;

    private:
        AudioStreamPtr stream {};

    };
    
}

#endif // SDL_AUDIO_STREAM_HH
