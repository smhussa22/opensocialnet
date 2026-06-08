// related headers
#include "AudioPlc.hh"

// c sys headers

// cpp stdlib headers
#include <algorithm>
#include <cstddef>

// 3rd party headers

// project headers
#include "AudioDecoder.hh"

namespace OpenSocialNet::Plc
{

    namespace
    {

        // Past this many consecutive concealments any strategy starts
        // producing artifacts worse than silence — opus PLC drifts, Repeat
        // sounds like a stuck syllable, Interpolate has nothing real to
        // anchor against. Past the cap conceal() returns 0 and lets SDL
        // silence-fill the slot instead.
        constexpr int max_consecutive_concealments { 5 };

    }


    AudioPlc::AudioPlc(PlcStrategy strategy, OpenSocialNet::Audio::AudioDecoder& decoder) noexcept : m_strategy { strategy }, m_decoder { decoder }
    {

        // m_last_frame / m_next_frame_hint default-initialise to zeroes,
        // m_have_*_frame flip true once real data arrives.

    }

    void AudioPlc::on_real_frame(std::span<const float> pcm) noexcept
    {

        // Copy into the last-frame buffer for Repeat / Interpolate to read
        // from on the next concealment. Caller may hand us a different size
        // than samples_per_frame (resampling, partial reads); only copy as
        // much as fits and pad the rest with zeros so the buffer doesn't
        // carry stale tail data into the next strategy invocation.
        const std::size_t copy_count { std::min(pcm.size(), m_last_frame.size()) };
        std::copy_n(pcm.begin(), copy_count, m_last_frame.begin());
        if (copy_count < m_last_frame.size()) std::fill(m_last_frame.begin() + copy_count, m_last_frame.end(), 0.0f);

        m_have_last_frame          = true;
        // A real frame breaks the streak — next conceal() starts fresh
        // against the cap budget.
        m_consecutive_concealments = 0;

    }

    int AudioPlc::conceal(std::span<float> out) noexcept
    {

        // Runaway-hallucination guard: return 0 and let SDL silence-fill.
        if (m_consecutive_concealments >= max_consecutive_concealments) return 0;

        int samples_written { 0 };
        switch (m_strategy)
        {

            case PlcStrategy::Silence:     samples_written = conceal_silence(out); break;
            case PlcStrategy::Repeat:      samples_written = conceal_repeat (out); break;
            case PlcStrategy::Opus:        samples_written = conceal_opus   (out); break;
            case PlcStrategy::Interpolate: samples_written = conceal_interp (out); break;

        }

        // The next-frame hint is per-concealment; clearing here means
        // Interpolate has to be re-hinted by the playback callback before
        // every call (which is what we want — stale hints across multiple
        // concealments would blend in the wrong direction).
        m_have_next_hint = false;

        if (samples_written > 0)
        {

            ++m_consecutive_concealments;
            ++m_total_concealments;

        }

        return samples_written;

    }

    void AudioPlc::hint_next_frame(std::span<const float> pcm) noexcept
    {

        const std::size_t copy_count { std::min(pcm.size(), m_next_frame_hint.size()) };
        std::copy_n(pcm.begin(), copy_count, m_next_frame_hint.begin());
        if (copy_count < m_next_frame_hint.size()) std::fill(m_next_frame_hint.begin() + copy_count, m_next_frame_hint.end(), 0.0f);
        m_have_next_hint = true;

    }

    std::uint64_t AudioPlc::concealments_emitted() const noexcept
    {

        return m_total_concealments;

    }

    PlcStrategy AudioPlc::strategy() const noexcept
    {

        return m_strategy;

    }


    // ---- per-strategy bodies ----

    // A. Silence: the trivial baseline. Emits a frame of zeros. Audible as
    // a stutter; useful as the "lower bound" reference when benchmarking
    // the other three.
    int AudioPlc::conceal_silence(std::span<float> out) noexcept
    {

        std::fill(out.begin(), out.end(), 0.0f);
        return static_cast<int>(out.size());

    }

    // B. Repeat: replays the most recent decoded frame. Cheap, sounds OK
    // for a single lost packet (extends one syllable by 10ms); past 2-3
    // consecutive losses the same syllable starts ringing.
    int AudioPlc::conceal_repeat(std::span<float> out) noexcept
    {

        if (!m_have_last_frame) return 0;

        const std::size_t copy_count { std::min(out.size(), m_last_frame.size()) };
        std::copy_n(m_last_frame.begin(), copy_count, out.begin());
        if (copy_count < out.size()) std::fill(out.begin() + copy_count, out.end(), 0.0f);
        return static_cast<int>(copy_count);

    }

    // C. Opus built-in PLC. Forwards to AudioDecoder::decode_missing_bytes,
    // which calls opus_decode_float(decoder, nullptr, 0, ...). The decoder
    // extrapolates from its internal history using spectral synthesis —
    // sounds noticeably better than Repeat at the cost of one extra opus
    // call per concealment. decode_missing_bytes requires the output span
    // to be exactly samples_per_frame; we slice out to that size if the
    // caller hands us more.
    int AudioPlc::conceal_opus(std::span<float> out) noexcept
    {

        if (out.size() < OpenSocialNet::Audio::samples_per_frame) return 0;

        const std::span<float> frame_slice { out.data(), OpenSocialNet::Audio::samples_per_frame };
        const int              decoded     { m_decoder.decode_missing_bytes(frame_slice) };
        return decoded > 0 ? decoded : 0;

    }

    // D. Linear interpolation between the previous good frame and the next
    // good frame's hint. Falls back to Repeat-style when no hint is
    // available (which is the common case for the very first conceal of
    // any gap, since hint_next_frame is only set when the receiver can
    // peek ahead). Goals.md "strategy D" — the depth-arc piece.
    //
    // Mix is per-sample linear crossfade:
    //   out[i] = (1 - w) * last_frame[i] + w * next_frame_hint[i]
    // with w = i / (n - 1) so the first sample reads pure last_frame
    // and the last sample reads pure next_frame_hint. Real depth-arc
    // upgrade later: cosine envelope + overlap-add for continuity, or
    // shift the source window so we draw from last_frame's tail rather
    // than its head.
    int AudioPlc::conceal_interp(std::span<float> out) noexcept
    {

        if (!m_have_last_frame) return 0;

        // No look-ahead available → behave like Repeat. Better than
        // silence; preserves at least the last good frame's content.
        if (!m_have_next_hint)
        {

            const std::size_t copy_count { std::min(out.size(), m_last_frame.size()) };
            std::copy_n(m_last_frame.begin(), copy_count, out.begin());
            if (copy_count < out.size()) std::fill(out.begin() + copy_count, out.end(), 0.0f);
            return static_cast<int>(copy_count);

        }

        const std::size_t n { std::min({ out.size(), m_last_frame.size(), m_next_frame_hint.size() }) };
        if (n == 0) return 0;
        const float denom { (n > 1) ? static_cast<float>(n - 1) : 1.0f };

        for (std::size_t i { 0 }; i < n; ++i)
        {

            const float w { static_cast<float>(i) / denom };
            out[i] = (1.0f - w) * m_last_frame[i] + w * m_next_frame_hint[i];

        }
        if (n < out.size()) std::fill(out.begin() + n, out.end(), 0.0f);

        return static_cast<int>(n);

    }

}
