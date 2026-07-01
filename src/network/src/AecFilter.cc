// related headers
#include "AecFilter.hh"

// c sys headers
#include <algorithm>
#include <cstdio>

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Network
{

    bool AecFilter::init(int frame_size, int sample_rate, int filter_length) noexcept
    {

        destroy();

        m_echo = ::speex_echo_state_init(frame_size, filter_length);
        if (m_echo == nullptr)
        {

            std::printf("[aec] speex_echo_state_init failed\n");
            return false;

        }

        ::speex_echo_ctl(m_echo, SPEEX_ECHO_SET_SAMPLING_RATE, &sample_rate);

        m_pp = ::speex_preprocess_state_init(frame_size, sample_rate);
        if (m_pp == nullptr)
        {

            std::printf("[aec] speex_preprocess_state_init failed\n");
            ::speex_echo_state_destroy(m_echo);
            m_echo = nullptr;
            return false;

        }

        // Link the echo state so the preprocessor can suppress residual echo.
        ::speex_preprocess_ctl(m_pp, SPEEX_PREPROCESS_SET_ECHO_STATE, m_echo);

        m_frame_size = frame_size;
        m_ref_i16.resize(static_cast<std::size_t>(frame_size));
        m_mic_i16.resize(static_cast<std::size_t>(frame_size));
        m_out_i16.resize(static_cast<std::size_t>(frame_size));

        std::printf("[aec] init ok: frame=%d sample_rate=%d filter=%d\n", frame_size, sample_rate, filter_length);
        return true;

    }

    void AecFilter::destroy() noexcept
    {

        if (m_pp   != nullptr) { ::speex_preprocess_state_destroy(m_pp);   m_pp   = nullptr; }
        if (m_echo != nullptr) { ::speex_echo_state_destroy(m_echo);       m_echo = nullptr; }

    }

    void AecFilter::notify_playback(const float* ref, int n) noexcept
    {

        if (m_echo == nullptr) return;
        const int count { std::min(n, m_frame_size) };
        for (int i { 0 }; i < count; ++i) m_ref_i16[static_cast<std::size_t>(i)] = float_to_s16(ref[i]);
        ::speex_echo_playback(m_echo, m_ref_i16.data());

    }

    void AecFilter::process(const float* mic, float* out, int n) noexcept
    {

        if (m_echo == nullptr)
        {

            if (mic != out) std::copy(mic, mic + n, out);
            return;

        }

        const int count { std::min(n, m_frame_size) };
        for (int i { 0 }; i < count; ++i) m_mic_i16[static_cast<std::size_t>(i)] = float_to_s16(mic[i]);

        ::speex_echo_capture(m_echo, m_mic_i16.data(), m_out_i16.data());
        ::speex_preprocess_run(m_pp, m_out_i16.data());

        for (int i { 0 }; i < count; ++i) out[i] = s16_to_float(m_out_i16[static_cast<std::size_t>(i)]);

    }

    std::int16_t AecFilter::float_to_s16(float x) noexcept
    {

        const float clamped { std::clamp(x, -1.0f, 1.0f) };
        return static_cast<std::int16_t>(clamped * 32767.0f);

    }

    float AecFilter::s16_to_float(std::int16_t x) noexcept
    {

        return static_cast<float>(x) * (1.0f / 32768.0f);

    }

}
