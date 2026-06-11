#ifndef OSN_VIDEO_PLC_HH
#define OSN_VIDEO_PLC_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <vector>

// 3rd party headers

// project headers

namespace OpenSocialNet::Plc
{

    // Which strategy this video PLC instance uses. Picked at construction
    // (from an env var in the receive path) so the per-frame path stays
    // branch-free past the initial dispatch. Bench is
    // `OSN_VIDEO_PLC=skip|hold|interp|motion`.
    enum class VideoPlcStrategy : std::uint8_t
    {

        Skip              = 0, // emit nothing — the renderer keeps showing the last presented frame
        Hold              = 1, // re-emit the last decoded frame explicitly
        Interpolate       = 2, // per-pixel blend between the last frame and the next good frame's hint
        MotionCompensated = 3, // block-matching motion between the last two real frames, extrapolated forward

    };


    // Single source of concealed video frames for the render path when the
    // jitter buffer / reassembler can't produce a complete frame in time.
    // Operates post-decode on YUV420P planes (the same place AudioPlc
    // operates on decoded PCM) — H.264 has no opus-style "conceal this
    // frame" decoder entry point, so every strategy here is synthesised
    // from previously decoded frames.
    //
    // Threading: the video receive/render thread owns one instance.
    // on_real_frame() and conceal() are both called from that thread so no
    // synchronisation is needed across them.
    class VideoPlc
    {

    public:

        explicit VideoPlc(VideoPlcStrategy strategy) noexcept;

        VideoPlc(const VideoPlc&)            = delete;
        VideoPlc& operator=(const VideoPlc&) = delete;
        VideoPlc(VideoPlc&&)                 = delete;
        VideoPlc& operator=(VideoPlc&&)      = delete;


        // Called once per real decoded frame. Rotates the last frame into
        // the previous-frame slot (MotionCompensated estimates between the
        // two), copies the new planes in, and resets the consecutive-
        // concealments counter so the next loss starts fresh. A resolution
        // change invalidates all stored history.
        void on_real_frame(const std::uint8_t* const* yuv420p_planes, const int* strides, int width, int height) noexcept;


        // For Interpolate: peek the next real frame the jitter buffer is
        // about to release so the gap can be bridged from both sides. The
        // hint is consumed by the next conceal() call; strategies that
        // don't need lookahead ignore it.
        void hint_next_frame(const std::uint8_t* const* yuv420p_planes, const int* strides, int width, int height) noexcept;


        // Called when no real frame is available for the current display
        // slot. On success points `yuv420p_planes` / `strides` at an
        // internally-owned YUV420P frame (valid until the next conceal() /
        // on_real_frame()) and returns true. Returns false when the
        // strategy emits nothing (Skip always; others when out of history
        // or past the consecutive cap) — the renderer should then keep its
        // last presented frame.
        bool conceal(std::uint8_t** yuv420p_planes, int* strides) noexcept;


        // Observable for stats / benchmarking. Read from the receive
        // loop's stats printer.
        std::uint64_t concealments_emitted() const noexcept;

        // Current strategy. Useful for logging at startup ("[video-plc]
        // strategy=motion") so a benchmark run is self-documenting.
        VideoPlcStrategy strategy() const noexcept;

        // Raise/lower the runaway-hallucination guard. Default 5 — Hold
        // freezes visibly and MotionCompensated smears past that. Bench
        // harness raises it so every loss event produces a measurable
        // concealment regardless of streak length.
        void set_max_consecutive_concealments(int n) noexcept;

        // Dimensions of the stored last frame (0 until the first real
        // frame arrives).
        [[nodiscard]] int width() const noexcept;
        [[nodiscard]] int height() const noexcept;


    private:

        // Packed YUV420P frame storage: luma stride == width, chroma
        // stride == width / 2. Decoder planes arrive with arbitrary
        // strides; copy_from() repacks them tight.
        struct YuvFrame
        {

            std::vector<std::uint8_t> y_plane {};      // packed luma, width * height bytes
            std::vector<std::uint8_t> u_plane {};      // packed chroma U, (width / 2) * (height / 2) bytes
            std::vector<std::uint8_t> v_plane {};      // packed chroma V, same size as U
            int                       frame_width  { 0 }; // luma width in pixels
            int                       frame_height { 0 }; // luma height in pixels
            bool                      valid { false };    // false until first copy_from / resize fill

            void copy_from(const std::uint8_t* const* planes, const int* strides, int width, int height);
            void resize(int width, int height);
            void expose(std::uint8_t** planes_out, int* strides_out) noexcept;

        };

        // One per 16x16 luma block: displacement of that block's content
        // from the previous frame to the last frame (i.e. per-frame motion,
        // extrapolated forward during concealment).
        struct MotionVector
        {

            std::int16_t dx { 0 }; // horizontal motion in luma pixels per frame
            std::int16_t dy { 0 }; // vertical motion in luma pixels per frame

        };


        // Per-strategy bodies. conceal() picks one via switch; the
        // bookkeeping (consecutive cap, total counter, hint reset) is owned
        // by conceal() so the bodies stay slim.
        bool conceal_hold  (std::uint8_t** planes, int* strides) noexcept;
        bool conceal_interp(std::uint8_t** planes, int* strides) noexcept;
        bool conceal_motion(std::uint8_t** planes, int* strides) noexcept;

        // Three-step block-matching search on the luma plane between
        // m_prev_frame and m_last_frame. Fills m_motion_vectors; called
        // once per loss gap (first conceal of a streak) and reused for
        // consecutive concealments by scaling the vectors.
        void estimate_motion() noexcept;


        VideoPlcStrategy m_strategy;                              // chosen at construction

        YuvFrame m_last_frame {};                                 // most recent real decoded frame (Hold, Interpolate, MotionCompensated read from here)
        YuvFrame m_prev_frame {};                                 // the real frame before that (MotionCompensated's estimation reference)
        YuvFrame m_next_frame_hint {};                            // peeked next frame (Interpolate only)
        YuvFrame m_conceal_frame {};                              // output storage exposed by conceal()
        bool     m_have_next_hint { false };                      // resets after each conceal() call

        std::vector<MotionVector> m_motion_vectors {};            // one per 16x16 luma block, refreshed per loss gap

        int           m_consecutive_concealments     { 0 };       // bumped per emitted conceal(), zeroed per on_real_frame()
        int           m_max_consecutive_concealments { 5 };       // cap; bench overrides to a large value to remove the streak guard
        std::uint64_t m_total_concealments           { 0 };       // monotonic across the session for stats

    };

}

#endif // OSN_VIDEO_PLC_HH
