// related headers

// c sys headers

// cpp stdlib headers
#include <memory>
#include <stdexcept>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "AudioStream.hh"


namespace OpenSocialNet::Audio
{

    AudioStream::AudioStream(const SDL_AudioSpec& spec, SDL_AudioDeviceID device, SDL_AudioStreamCallback callback, void* userdata) : stream { SDL_OpenAudioDeviceStream(device, &spec, callback, userdata) }
    {

        if (!stream) throw std::runtime_error { SDL_GetError() };

    }

    void AudioStream::resume() noexcept
    {

        SDL_ResumeAudioStreamDevice(stream.get());

    }

    void AudioStream::pause() noexcept
    {

        SDL_PauseAudioStreamDevice(stream.get());

    }

    void AudioStream::put_audio_data(const void* data, int size) noexcept
    {

        SDL_PutAudioStreamData(stream.get(), data, size);

    }

}

