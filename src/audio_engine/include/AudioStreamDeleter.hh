#ifndef SDL_AUDIO_STREAM_DELETER_HH
#define SDL_AUDIO_STREAM_DELETER_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers

namespace OpenSocialNet::Audio
{

    struct AudioStreamDeleter
    {

        void operator()(SDL_AudioStream* stream) const
        {

            if (stream != nullptr) SDL_DestroyAudioStream(stream);

        }

    };

    using AudioStreamPtr = std::unique_ptr<SDL_AudioStream, AudioStreamDeleter>;
    
}

#endif // SDL_AUDIO_STREAM_DELETER_HH
