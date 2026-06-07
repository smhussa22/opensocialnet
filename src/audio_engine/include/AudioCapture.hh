#ifndef AUDIO_CAPTURE_HH
#define AUDIO_CAPTURE_HH

// c sys headers
#include <cstddef>

// cpp stdlib headers
#include <optional>
#include <span>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "AudioStream.hh"
#include "RingBuffer.hh"
#include "AudioConstants.hh"

namespace OpenSocialNet::Audio
{
    class AudioCapture
    {
    public:
        AudioCapture() = default;
        ~AudioCapture() { shutdown(); }

        AudioCapture(const AudioCapture&) = delete;
        AudioCapture& operator=(const AudioCapture&) = delete;
        AudioCapture(AudioCapture&&) = delete;
        AudioCapture& operator=(AudioCapture&&) = delete;

        // opens SDL capture device and starts recording.
        // device_id defaults to SDL_AUDIO_DEVICE_DEFAULT_RECORDING; pass a
        // specific id (looked up via SDL_GetAudioRecordingDevices) when the
        // user has told us which mic to use via OSN_AUDIO_INPUT.
        // returns true on success, false if the device fails to open
        bool init(SDL_AudioDeviceID device_id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING) noexcept;

        // pauses and closes the capture device
        void shutdown() noexcept;

        // reads samples from internal ring buffer into destination
        // returns number of frames actually read, called by sender thread
        size_t read(std::span<float> destination) noexcept;

        // returns how many samples are ready to read
        size_t available() const noexcept;

    private:
        // called by SDL when mic data is ready — writes into capture_buffer
        static void onCapture(void* userdata, SDL_AudioStream* stream, int additional, int total);
        std::optional<AudioStream> stream {};
        AudioRingBuffer capture_buffer {};

    };

}

#endif // AUDIO_CAPTURE_HH