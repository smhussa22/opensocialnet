#ifndef OSN_AUDIO_PLC_HH
#define OSN_AUDIO_PLC_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <array>
#include <span>

// 3rd party headers

// project headers
#include "AudioConstants.hh"

namespace OpenSocialNet::Audio
{

    class AudioDecoder;

}

namespace OpenSocialNet::Plc
{

    // Which strategy this PLC instance uses. Picked at construction (from an
    // env var in main.cc) so the audio callback path stays branch-free past
    // the initial dispatch. Bench is `OSN_PLC=silence|repeat|opus|interp`.
    enum class PlcStrategy : std::uint8_t
    {

        Silence     = 0, // emit a frame of zeros — the trivial baseline
        Repeat      = 1, // replay the last decoded PCM buffer
        Opus        = 2, // ask opus_decode(decoder, nullptr, 0, ...) for its built-in concealer
        Interpolate = 3, // blend the tail of the last good frame with the head of the next good frame (depth-arc strategy D from goals.md)

    };


    // Single source of audio frames for the SDL playback callback when the
    // jitter buffer has no real packet to give. Owns "last good frame"
    // state so Repeat / Interpolate / Opus's history have something to work
    // against, and counts concealments emitted so the [net-stats] line can
    // surface PLC volume per run.
    //
    // Threading: the SDL audio callback owns one instance for the playback
    // path. on_real_frame() and conceal() are both called from that callback
    // so no synchronisation is needed across them.
    class AudioPlc
    {

    public:

        AudioPlc(PlcStrategy strategy, OpenSocialNet::Audio::AudioDecoder& decoder) noexcept;

        AudioPlc(const AudioPlc&)            = delete;
        AudioPlc& operator=(const AudioPlc&) = delete;
        AudioPlc(AudioPlc&&)                 = delete;
        AudioPlc& operator=(AudioPlc&&)      = delete;


        // Called once per real packet that the jitter buffer hands back to
        // the audio callback. Lets the PLC refresh its "last good frame"
        // state (used by Repeat + Interpolate) and reset the consecutive-
        // concealments counter so the next loss starts fresh.
        void on_real_frame(std::span<const float> pcm) noexcept;


        // Called when the jitter buffer can't provide a real packet for the
        // current playback slot. Fills `out` with whatever the chosen
        // strategy decided and returns the number of samples written.
        // Returns 0 when the strategy decides to give up — SDL will then
        // silence-fill the slot. The consecutive-concealments counter caps
        // runaway hallucination (opus PLC drifts after ~3 frames; Repeat
        // sounds like a stuck syllable past ~5).
        int conceal(std::span<float> out) noexcept;


        // For interpolate: peek the next real frame the jitter buffer is
        // about to release so we can blend its head with our last frame's
        // tail. Called by the SDL playback callback after pop() fails but
        // before conceal(); the implementation may ignore the hint for
        // strategies that don't need lookahead.
        void hint_next_frame(std::span<const float> pcm) noexcept;


        // Observable for stats / benchmarking. Read from the main loop's
        // [net-stats] printer.
        std::uint64_t concealments_emitted() const noexcept;

        // Current strategy. Useful for logging at startup ("[plc] strategy=
        // opus") so a benchmark run is self-documenting.
        PlcStrategy strategy() const noexcept;

        // Raise/lower the runaway-hallucination guard. Default 5 — opus PLC
        // drifts and Repeat ringt after that. Bench harness raises it to
        // effectively unlimited so every loss event produces a measurable
        // concealment regardless of streak length.
        void set_max_consecutive_concealments(int n) noexcept;


    private:

        // Per-strategy bodies. conceal() picks one of these via switch and
        // returns whatever it returns. Each is responsible for one strategy
        // only; the bookkeeping (consecutive cap, total counter) is owned
        // by conceal() so the bodies stay slim.
        int conceal_silence (std::span<float> out) noexcept;
        int conceal_repeat  (std::span<float> out) noexcept;
        int conceal_opus    (std::span<float> out) noexcept;
        int conceal_interp  (std::span<float> out) noexcept;


        PlcStrategy m_strategy;                                                                          // chosen at construction
        OpenSocialNet::Audio::AudioDecoder& m_decoder;                                                   // borrowed; needed by Opus strategy for its PLC entry point

        std::array<float, OpenSocialNet::Audio::samples_per_frame> m_last_frame {};                      // most recent real PCM frame (Repeat, Interpolate read from here)
        std::array<float, OpenSocialNet::Audio::samples_per_frame> m_next_frame_hint {};                 // peeked next frame (Interpolate only)
        bool                                                       m_have_last_frame { false };         // false until first real frame arrives
        bool                                                       m_have_next_hint  { false };         // resets after each conceal() call

        int                                                        m_consecutive_concealments     { 0 };    // bumped per conceal(), zeroed per on_real_frame()
        int                                                        m_max_consecutive_concealments { 5 };    // cap; bench overrides to a large value to remove the streak guard
        std::uint64_t                                              m_total_concealments           { 0 };    // monotonic across the session for [net-stats]

    };

}

#endif // OSN_AUDIO_PLC_HH
