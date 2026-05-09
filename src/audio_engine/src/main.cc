// related headers

// c sys headers
#include <cmath>
#include <cstdint>

// cpp stdlib headers
#include <iostream>
#include <vector>

// 3rd party headers
#include <SDL3/SDL.h>

// project headers
#include "AudioStream.hh"
#include "AudioConstants.hh"

int main()
{

    if (!SDL_Init(SDL_INIT_AUDIO))
    {

        std::cout << "SDL initialization failed: " << SDL_GetError();
        return -1;

    }

    {

        SDL_AudioSpec audio_spec { OpenSocialNet::Audio::create_opus_audio_spec() };
        OpenSocialNet::Audio::AudioStream audio_stream { audio_spec };
        audio_stream.resume();

        // Generate and push 3 seconds of 440Hz sine wave
        const int sampleRate = 48000;
        const int seconds    = 3;
        const int numSamples = sampleRate * seconds;
        const float freq     = 440.0f;
        const float amp      = 0.3f;

        float phase = 0.0f;
        const float phaseInc = 2.0f * M_PI * freq / sampleRate;

        std::vector<float> buffer(numSamples);
        for (int i = 0; i < numSamples; ++i) {
            buffer[i] = amp * std::sin(phase);
            phase += phaseInc;
            if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
        }

        audio_stream.put_audio_data(buffer.data(), numSamples * sizeof(float));

        std::cout << "Playing 440Hz tone for 3 seconds...\n";
        SDL_Delay(3500);

    }

    SDL_Quit();

    return 0;

}