// related headers
#include "AudioStream.hh"

// c sys headers

// cpp stdlib headers
#include <memory>
#include <stdexcept>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers

namespace OpenSocialNet::Audio
{

    AudioStream::AudioStream(const SDL_AudioSpec& audio_spec) : m_stream { SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr) }
    {

        if (!m_stream) throw std::runtime_error { SDL_GetError() };

    }

    void AudioStream::resume() noexcept
    {

        SDL_ResumeAudioStreamDevice(m_stream.get());

    }

    void AudioStream::pause() noexcept
    {

        SDL_PauseAudioStreamDevice(m_stream.get());

    }

    void AudioStream::put_audio_data(const void* data, int size) noexcept
    {

        SDL_PutAudioStreamData(m_stream.get(), data, size);

    }

}

