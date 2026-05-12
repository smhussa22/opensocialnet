// related headers
#include "AudioCapture.hh"

// c sys headers
#include <cstddef>

// cpp stdlib headers
#include <stdexcept>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "RingBuffer.hh"
#include "AudioConstants.hh"

namespace OpenSocialNet::Audio
{

    bool AudioCapture::init() noexcept
    {

        try
        {

            SDL_AudioSpec audio_spec { create_opus_audio_spec() };
            stream.emplace(audio_spec, SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &AudioCapture::onCapture, this);
            stream->resume();
            return true;

        }
        catch(const std::exception& e)
        {

            return false;

        }
        
    }

    void AudioCapture::shutdown() noexcept
    {

        if (stream) stream->pause();
        stream.reset();

    }

    size_t AudioCapture::read(std::span<float> destination) noexcept
    {

        return capture_buffer.read(destination.data(), destination.size());

    }

    size_t AudioCapture::available() const noexcept
    {

        return capture_buffer.available();

    }

    void AudioCapture::onCapture(void* userdata, SDL_AudioStream* stream, int additional, int total)
    {

        AudioCapture* self = static_cast<AudioCapture*>(userdata);

        std::array<float, 4096> temp {};
        int bytes_to_read = std::min(additional, static_cast<int>(sizeof(temp)));
        int bytes_read = SDL_GetAudioStreamData(stream, temp.data(), bytes_to_read);
        if (bytes_read <= 0) return;

        self->capture_buffer.write(temp.data(), bytes_read / sizeof(float));

    }

}