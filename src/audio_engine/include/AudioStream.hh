#ifndef SDL_AUDIO_STREAM_HH
#define SDL_AUDIO_STREAM_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "AudioStreamDeleter.hh"

namespace OpenSocialNet::Audio
{

    class AudioStream
    {
    public:
        explicit AudioStream(const SDL_AudioSpec& audio_spec);
        void resume() noexcept;
        void pause() noexcept;
        void put_audio_data(const void* data, int size) noexcept;

    private:
        AudioStreamPtr m_stream {};

    };
    
}

#endif // SDL_AUDIO_STREAM_HH
