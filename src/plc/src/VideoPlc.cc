// related headers
#include "VideoPlc.hh"

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <algorithm>
#include <cstring>

// 3rd party headers

// project headers

namespace
{

    // Block-matching parameters for MotionCompensated. 16x16 luma blocks
    // (the H.264 macroblock size, so vectors line up with what the encoder
    // was tracking) searched with a three-step search: step sizes 4 -> 2
    // -> 1 reach +-7 pixels of motion per frame at ~25 SAD evaluations per
    // block instead of the 225 a full +-7 search would cost.
    constexpr int luma_block_size { 16 };
    constexpr int initial_search_step { 4 };

    int clamp_coord(int v, int lo, int hi) noexcept
    {

        return std::clamp(v, lo, hi);

    }

    // Sum of absolute differences between a block of `cur` and the same
    // block of `ref` displaced by (-dx, -dy), i.e. scoring the hypothesis
    // "this content moved by (dx, dy) from ref to cur". Reference reads
    // are clamped to the plane so edge blocks score against smeared
    // borders rather than reading out of bounds.
    std::uint32_t block_sad(const std::uint8_t* cur, const std::uint8_t* ref, int width, int height, int bx, int by, int bw, int bh, int dx, int dy) noexcept
    {

        std::uint32_t sad { 0 };
        for (int row { 0 }; row < bh; ++row)
        {

            const int cy { by + row };
            const int ry { clamp_coord(cy - dy, 0, height - 1) };
            const std::uint8_t* cur_row { cur + static_cast<std::size_t>(cy) * width };
            const std::uint8_t* ref_row { ref + static_cast<std::size_t>(ry) * width };
            for (int col { 0 }; col < bw; ++col)
            {

                const int cx { bx + col };
                const int rx { clamp_coord(cx - dx, 0, width - 1) };
                const int diff { static_cast<int>(cur_row[cx]) - static_cast<int>(ref_row[rx]) };
                sad += static_cast<std::uint32_t>(diff < 0 ? -diff : diff);

            }

        }
        return sad;

    }

    // Copy one plane out of arbitrary-stride decoder output into packed
    // storage (stride == row width).
    void pack_plane(std::uint8_t* dst, const std::uint8_t* src, int src_stride, int row_bytes, int rows)
    {

        for (int row { 0 }; row < rows; ++row) std::memcpy(dst + static_cast<std::size_t>(row) * row_bytes, src + static_cast<std::size_t>(row) * src_stride, static_cast<std::size_t>(row_bytes));

    }

    // Fetch a plane pixel with the source coordinate displaced by the
    // (scaled) motion vector and clamped to the plane bounds.
    std::uint8_t sample_displaced(const std::uint8_t* plane, int width, int height, int x, int y, int dx, int dy) noexcept
    {

        const int sx { clamp_coord(x - dx, 0, width - 1) };
        const int sy { clamp_coord(y - dy, 0, height - 1) };
        return plane[static_cast<std::size_t>(sy) * width + sx];

    }

}

namespace OpenSocialNet::Plc
{

    // ---- YuvFrame ----

    void VideoPlc::YuvFrame::copy_from(const std::uint8_t* const* planes, const int* strides, int width, int height)
    {

        resize(width, height);
        const int chroma_width { width / 2 };
        const int chroma_height { height / 2 };
        pack_plane(y_plane.data(), planes[0], strides[0], width, height);
        pack_plane(u_plane.data(), planes[1], strides[1], chroma_width, chroma_height);
        pack_plane(v_plane.data(), planes[2], strides[2], chroma_width, chroma_height);

    }

    void VideoPlc::YuvFrame::resize(int width, int height)
    {

        frame_width = width;
        frame_height = height;
        y_plane.resize(static_cast<std::size_t>(width) * height);
        u_plane.resize(static_cast<std::size_t>(width / 2) * (height / 2));
        v_plane.resize(u_plane.size());
        valid = true;

    }

    void VideoPlc::YuvFrame::expose(std::uint8_t** planes_out, int* strides_out) noexcept
    {

        planes_out[0] = y_plane.data();
        planes_out[1] = u_plane.data();
        planes_out[2] = v_plane.data();
        strides_out[0] = frame_width;
        strides_out[1] = frame_width / 2;
        strides_out[2] = frame_width / 2;

    }


    // ---- VideoPlc ----

    VideoPlc::VideoPlc(VideoPlcStrategy strategy) noexcept : m_strategy { strategy }
    {

        // Frame slots stay invalid until real frames arrive; every
        // strategy except Skip degrades to "emit nothing" until then.

    }

    void VideoPlc::on_real_frame(const std::uint8_t* const* yuv420p_planes, const int* strides, int width, int height) noexcept
    {

        if (width <= 0 || height <= 0) return;

        // A resolution change makes motion estimation and blending against
        // older frames meaningless — drop all history and start over.
        if (m_last_frame.valid && (m_last_frame.frame_width != width || m_last_frame.frame_height != height))
        {

            m_prev_frame.valid = false;
            m_next_frame_hint.valid = false;
            m_have_next_hint = false;

        }

        // Rotate last -> prev (vector swap, no copy) so MotionCompensated
        // always has the two most recent real frames to estimate between.
        if (m_last_frame.valid) std::swap(m_prev_frame, m_last_frame);
        m_last_frame.copy_from(yuv420p_planes, strides, width, height);

        // A real frame breaks the streak — next conceal() starts fresh
        // against the cap budget.
        m_consecutive_concealments = 0;

    }

    void VideoPlc::hint_next_frame(const std::uint8_t* const* yuv420p_planes, const int* strides, int width, int height) noexcept
    {

        if (width <= 0 || height <= 0) return;
        m_next_frame_hint.copy_from(yuv420p_planes, strides, width, height);
        m_have_next_hint = true;

    }

    bool VideoPlc::conceal(std::uint8_t** yuv420p_planes, int* strides) noexcept
    {

        // Runaway-hallucination guard: stop emitting and let the renderer
        // freeze on its last presented frame.
        if (m_consecutive_concealments >= m_max_consecutive_concealments) return false;

        bool produced { false };
        switch (m_strategy)
        {

            case VideoPlcStrategy::Skip:              produced = false;                                   break;
            case VideoPlcStrategy::Hold:              produced = conceal_hold  (yuv420p_planes, strides); break;
            case VideoPlcStrategy::Interpolate:       produced = conceal_interp(yuv420p_planes, strides); break;
            case VideoPlcStrategy::MotionCompensated: produced = conceal_motion(yuv420p_planes, strides); break;

        }

        // The next-frame hint is per-concealment; clearing here means
        // Interpolate has to be re-hinted before every call (stale hints
        // across multiple concealments would blend in the wrong direction).
        m_have_next_hint = false;

        if (produced)
        {

            ++m_consecutive_concealments;
            ++m_total_concealments;

        }

        return produced;

    }

    std::uint64_t VideoPlc::concealments_emitted() const noexcept
    {

        return m_total_concealments;

    }

    VideoPlcStrategy VideoPlc::strategy() const noexcept
    {

        return m_strategy;

    }

    void VideoPlc::set_max_consecutive_concealments(int n) noexcept
    {

        m_max_consecutive_concealments = n;

    }

    int VideoPlc::width() const noexcept
    {

        return m_last_frame.valid ? m_last_frame.frame_width : 0;

    }

    int VideoPlc::height() const noexcept
    {

        return m_last_frame.valid ? m_last_frame.frame_height : 0;

    }


    // ---- per-strategy bodies ----

    // B. Hold: re-emits the most recent decoded frame. Visually identical
    // to Skip on a plain renderer (both end up showing the same picture),
    // but Hold hands the caller real planes — needed when the downstream
    // sink consumes a frame per display slot (recorder, compositor) rather
    // than just repainting a texture.
    bool VideoPlc::conceal_hold(std::uint8_t** planes, int* strides) noexcept
    {

        if (!m_last_frame.valid) return false;

        m_last_frame.expose(planes, strides);
        return true;

    }

    // C. Interpolate: per-pixel blend between the last decoded frame and
    // the next good frame's hint. Falls back to Hold when no hint is
    // available (the common case for the first conceal of a gap, since
    // the hint only exists when the jitter buffer can peek ahead). The
    // blend weight drifts toward the hint as the streak grows —
    // w = (k + 1) / (k + 2) for the k-th consecutive concealment — so a
    // multi-frame gap eases into the upcoming frame instead of holding a
    // static 50/50 ghost.
    bool VideoPlc::conceal_interp(std::uint8_t** planes, int* strides) noexcept
    {

        if (!m_last_frame.valid) return false;

        const bool hint_usable { m_have_next_hint && m_next_frame_hint.valid && m_next_frame_hint.frame_width == m_last_frame.frame_width && m_next_frame_hint.frame_height == m_last_frame.frame_height };
        if (!hint_usable) return conceal_hold(planes, strides);

        // Fixed-point weight in [0, 256] — integer blend keeps this a few
        // adds and a shift per pixel across ~1.5 M pixels at 720p.
        const int weight { (m_consecutive_concealments + 1) * 256 / (m_consecutive_concealments + 2) };

        m_conceal_frame.resize(m_last_frame.frame_width, m_last_frame.frame_height);

        const auto blend_plane = [weight](const std::vector<std::uint8_t>& from, const std::vector<std::uint8_t>& to, std::vector<std::uint8_t>& out) noexcept
        {

            for (std::size_t i { 0 }; i < out.size(); ++i)
            {

                const int a { static_cast<int>(from[i]) };
                const int b { static_cast<int>(to[i]) };
                out[i] = static_cast<std::uint8_t>(a + (((b - a) * weight) >> 8));

            }

        };

        blend_plane(m_last_frame.y_plane, m_next_frame_hint.y_plane, m_conceal_frame.y_plane);
        blend_plane(m_last_frame.u_plane, m_next_frame_hint.u_plane, m_conceal_frame.u_plane);
        blend_plane(m_last_frame.v_plane, m_next_frame_hint.v_plane, m_conceal_frame.v_plane);

        m_conceal_frame.expose(planes, strides);
        return true;

    }

    // D. MotionCompensated: estimate per-block motion between the two most
    // recent real frames, then extrapolate that motion forward — the k-th
    // consecutive concealed frame samples the last frame displaced by
    // k * mv per block, so panning / moving content keeps moving through
    // the gap instead of freezing. Falls back to Hold until two real
    // frames of the same resolution exist.
    bool VideoPlc::conceal_motion(std::uint8_t** planes, int* strides) noexcept
    {

        if (!m_last_frame.valid) return false;

        const bool history_usable { m_prev_frame.valid && m_prev_frame.frame_width == m_last_frame.frame_width && m_prev_frame.frame_height == m_last_frame.frame_height };
        if (!history_usable) return conceal_hold(planes, strides);

        // Vectors are estimated once per loss gap (first conceal of the
        // streak) and re-scaled for the consecutive ones.
        if (m_consecutive_concealments == 0) estimate_motion();

        const int width { m_last_frame.frame_width };
        const int height { m_last_frame.frame_height };
        const int chroma_width { width / 2 };
        const int chroma_height { height / 2 };
        const int blocks_x { (width + luma_block_size - 1) / luma_block_size };
        const int scale { m_consecutive_concealments + 1 };

        m_conceal_frame.resize(width, height);

        // For every output pixel, fetch from the last real frame displaced
        // backwards along the block's (scaled) motion vector. Chroma
        // planes are quarter-size so coordinates and vectors halve.
        for (int y { 0 }; y < height; ++y)
        {

            const int block_row { y / luma_block_size };
            for (int x { 0 }; x < width; ++x)
            {

                const MotionVector& mv { m_motion_vectors[static_cast<std::size_t>(block_row) * blocks_x + x / luma_block_size] };
                m_conceal_frame.y_plane[static_cast<std::size_t>(y) * width + x] = sample_displaced(m_last_frame.y_plane.data(), width, height, x, y, mv.dx * scale, mv.dy * scale);

            }

        }
        for (int y { 0 }; y < chroma_height; ++y)
        {

            const int block_row { (y * 2) / luma_block_size };
            for (int x { 0 }; x < chroma_width; ++x)
            {

                const MotionVector& mv { m_motion_vectors[static_cast<std::size_t>(block_row) * blocks_x + (x * 2) / luma_block_size] };
                const int dx { (mv.dx * scale) / 2 };
                const int dy { (mv.dy * scale) / 2 };
                const std::size_t i { static_cast<std::size_t>(y) * chroma_width + x };
                m_conceal_frame.u_plane[i] = sample_displaced(m_last_frame.u_plane.data(), chroma_width, chroma_height, x, y, dx, dy);
                m_conceal_frame.v_plane[i] = sample_displaced(m_last_frame.v_plane.data(), chroma_width, chroma_height, x, y, dx, dy);

            }

        }

        m_conceal_frame.expose(planes, strides);
        return true;

    }

    void VideoPlc::estimate_motion() noexcept
    {

        const int width { m_last_frame.frame_width };
        const int height { m_last_frame.frame_height };
        const int blocks_x { (width + luma_block_size - 1) / luma_block_size };
        const int blocks_y { (height + luma_block_size - 1) / luma_block_size };
        m_motion_vectors.assign(static_cast<std::size_t>(blocks_x) * blocks_y, MotionVector {});

        const std::uint8_t* cur { m_last_frame.y_plane.data() };
        const std::uint8_t* ref { m_prev_frame.y_plane.data() };

        for (int by { 0 }; by < blocks_y; ++by)
        {

            for (int bx { 0 }; bx < blocks_x; ++bx)
            {

                const int px { bx * luma_block_size };
                const int py { by * luma_block_size };
                const int bw { std::min(luma_block_size, width - px) };
                const int bh { std::min(luma_block_size, height - py) };

                // Three-step search: start at zero motion, test the 8
                // neighbours at the current step around the best candidate,
                // halve the step, repeat. Zero-motion bias: ties keep the
                // existing best so static blocks stay put.
                int best_dx { 0 };
                int best_dy { 0 };
                std::uint32_t best_sad { block_sad(cur, ref, width, height, px, py, bw, bh, 0, 0) };

                for (int step { initial_search_step }; step >= 1; step /= 2)
                {

                    const int centre_dx { best_dx };
                    const int centre_dy { best_dy };
                    for (int ny { -1 }; ny <= 1; ++ny)
                    {

                        for (int nx { -1 }; nx <= 1; ++nx)
                        {

                            if (nx == 0 && ny == 0) continue;
                            const int dx { centre_dx + nx * step };
                            const int dy { centre_dy + ny * step };
                            const std::uint32_t sad { block_sad(cur, ref, width, height, px, py, bw, bh, dx, dy) };
                            if (sad < best_sad)
                            {

                                best_sad = sad;
                                best_dx = dx;
                                best_dy = dy;

                            }

                        }

                    }

                }

                m_motion_vectors[static_cast<std::size_t>(by) * blocks_x + bx] = MotionVector { static_cast<std::int16_t>(best_dx), static_cast<std::int16_t>(best_dy) };

            }

        }

    }

}
