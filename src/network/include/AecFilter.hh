#ifndef AEC_FILTER_HH
#define AEC_FILTER_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <vector>

// 3rd party headers
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

// project headers

namespace OpenSocialNet::Network
{

    // Acoustic echo canceller wrapping SpeexEchoState + SpeexPreprocessState.
    // Float API: internally converts float↔int16 before/after each Speex call.
    //
    // Typical usage:
    //   1. Call notify_playback() once per frame with what was just mixed to
    //      the speaker (the far-end reference signal).
    //   2. Call process() with the mic capture (near-end); it cancels the
    //      echo and writes the cleaned signal to `out`.
    //
    // Threading: not thread-safe — all methods must be called from the same
    // thread (the capture loop).
    class AecFilter
    {

    public:

        AecFilter() = default;
        ~AecFilter() { destroy(); }

        AecFilter(const AecFilter&)            = delete;
        AecFilter& operator=(const AecFilter&) = delete;
        AecFilter(AecFilter&&)                 = delete;
        AecFilter& operator=(AecFilter&&)      = delete;

        // frame_size: samples per frame (480 for 10ms at 48 kHz).
        // filter_length: tail length in samples (≈100ms → 4800 at 48 kHz).
        bool init(int frame_size, int sample_rate, int filter_length) noexcept;
        void destroy() noexcept;
        bool valid() const noexcept { return m_echo != nullptr; }

        // Feed the far-end reference (what was mixed to the speaker this frame).
        // Must be called with exactly frame_size floats.
        void notify_playback(const float* ref, int n) noexcept;

        // Cancel echo from mic. `mic` and `out` may point to the same buffer.
        // Must be called with exactly frame_size floats.
        void process(const float* mic, float* out, int n) noexcept;

    private:

        static std::int16_t float_to_s16(float x) noexcept;
        static float s16_to_float(std::int16_t x) noexcept;

        ::SpeexEchoState*       m_echo     { nullptr }; // core AEC state
        ::SpeexPreprocessState* m_pp       { nullptr }; // residual echo + noise suppression
        int                     m_frame_size { 0 };     // samples per frame, set at init
        std::vector<std::int16_t> m_ref_i16  { };       // temp buffer: float→int16 reference
        std::vector<std::int16_t> m_mic_i16  { };       // temp buffer: float→int16 near-end
        std::vector<std::int16_t> m_out_i16  { };       // temp buffer: AEC output

    };

}

#endif // AEC_FILTER_HH
