// Synthetic exercise of all four VideoPlc strategies: build two frames of
// a bright square panning across a flat background, lose the next frame,
// and check each strategy's concealment does what it claims — Skip emits
// nothing, Hold replays the last frame, Interpolate blends toward a hint,
// MotionCompensated keeps the square moving along its trajectory.

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <cstdio>
#include <vector>

// 3rd party headers

// project headers
#include "VideoPlc.hh"

namespace
{

    constexpr int width { 128 };
    constexpr int height { 96 };
    constexpr int square_size { 16 };
    constexpr std::uint8_t background_luma { 40 };
    constexpr std::uint8_t square_luma { 220 };

    struct TestFrame
    {

        std::vector<std::uint8_t> y; // packed luma plane
        std::vector<std::uint8_t> u; // packed chroma U plane
        std::vector<std::uint8_t> v; // packed chroma V plane

    };

    // Flat background with a bright square whose top-left corner sits at
    // (square_x, square_y).
    TestFrame make_frame(int square_x, int square_y)
    {

        TestFrame f { std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height, background_luma), std::vector<std::uint8_t>(static_cast<std::size_t>(width / 2) * (height / 2), 128), std::vector<std::uint8_t>(static_cast<std::size_t>(width / 2) * (height / 2), 128) };
        for (int row { 0 }; row < square_size; ++row)
        {

            for (int col { 0 }; col < square_size; ++col) f.y[static_cast<std::size_t>(square_y + row) * width + square_x + col] = square_luma;

        }
        return f;

    }

    void feed_real(OpenSocialNet::Plc::VideoPlc& plc, TestFrame& f)
    {

        const std::uint8_t* planes[3] { f.y.data(), f.u.data(), f.v.data() };
        const int strides[3] { width, width / 2, width / 2 };
        plc.on_real_frame(planes, strides, width, height);

    }

    void feed_hint(OpenSocialNet::Plc::VideoPlc& plc, TestFrame& f)
    {

        const std::uint8_t* planes[3] { f.y.data(), f.u.data(), f.v.data() };
        const int strides[3] { width, width / 2, width / 2 };
        plc.hint_next_frame(planes, strides, width, height);

    }

    std::uint8_t luma_at(const std::uint8_t* const* planes, const int* strides, int x, int y)
    {

        return planes[0][static_cast<std::size_t>(y) * strides[0] + x];

    }

    int failures { 0 };

    void check(bool ok, const char* what)
    {

        if (!ok) ++failures;
        std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);

    }

}

int main()
{

    // The square pans +4 px/frame horizontally, +2 px/frame vertically.
    TestFrame frame_a { make_frame(40, 40) };
    TestFrame frame_b { make_frame(44, 42) };
    TestFrame frame_d { make_frame(52, 46) }; // two frames ahead of b — used as the interpolate hint

    std::uint8_t* planes[3] {};
    int strides[3] {};

    // Skip: never emits, never counts.
    {

        std::printf("strategy=skip\n");
        OpenSocialNet::Plc::VideoPlc plc { OpenSocialNet::Plc::VideoPlcStrategy::Skip };
        feed_real(plc, frame_a);
        feed_real(plc, frame_b);
        check(!plc.conceal(planes, strides), "conceal emits nothing");
        check(plc.concealments_emitted() == 0, "nothing counted");

    }

    // Hold: byte-identical replay of the last real frame.
    {

        std::printf("strategy=hold\n");
        OpenSocialNet::Plc::VideoPlc plc { OpenSocialNet::Plc::VideoPlcStrategy::Hold };
        check(!plc.conceal(planes, strides), "no history -> emits nothing");
        feed_real(plc, frame_a);
        feed_real(plc, frame_b);
        check(plc.conceal(planes, strides), "conceal emits a frame");
        check(luma_at(planes, strides, 44, 42) == square_luma, "square still at frame b's position");
        check(luma_at(planes, strides, 40, 40) == background_luma, "frame a's position is background (it's b, not a)");
        check(plc.width() == width && plc.height() == height, "dimensions tracked");

        // The streak cap: 5 emits then nothing.
        for (int i { 0 }; i < 4; ++i) plc.conceal(planes, strides);
        check(!plc.conceal(planes, strides), "6th consecutive conceal blocked by cap");
        check(plc.concealments_emitted() == 5, "counted 5 concealments");

    }

    // Interpolate: with a hint two frames ahead, the first conceal blends
    // 50/50 — the square's old and new positions both read as mid-grey.
    {

        std::printf("strategy=interp\n");
        OpenSocialNet::Plc::VideoPlc plc { OpenSocialNet::Plc::VideoPlcStrategy::Interpolate };
        feed_real(plc, frame_a);
        feed_real(plc, frame_b);
        feed_hint(plc, frame_d);
        check(plc.conceal(planes, strides), "conceal emits a frame");
        const int blend_old { luma_at(planes, strides, 45, 43) };  // inside b's square, outside d's
        const int blend_new { luma_at(planes, strides, 65, 60) };  // inside d's square, outside b's
        const int mid { (background_luma + square_luma) / 2 };
        check(blend_old > mid - 12 && blend_old < mid + 12, "fading-out square reads ~50% blend");
        check(blend_new > mid - 12 && blend_new < mid + 12, "fading-in square reads ~50% blend");

        // No hint on the second conceal -> falls back to a hold.
        check(plc.conceal(planes, strides), "second conceal (no hint) still emits");
        check(luma_at(planes, strides, 45, 43) == square_luma, "fallback replays last frame untouched");

    }

    // MotionCompensated: square moved (+4, +2) between a and b, so the
    // concealed frame should place it around (48, 44).
    {

        std::printf("strategy=motion\n");
        OpenSocialNet::Plc::VideoPlc plc { OpenSocialNet::Plc::VideoPlcStrategy::MotionCompensated };
        feed_real(plc, frame_a);
        feed_real(plc, frame_b);
        check(plc.conceal(planes, strides), "conceal emits a frame");
        check(luma_at(planes, strides, 55, 50) == square_luma, "square advanced to extrapolated position (~48,44)");
        check(luma_at(planes, strides, 45, 43) == background_luma, "square left frame b's position");

        // Second consecutive conceal extrapolates twice as far (~52, 46).
        check(plc.conceal(planes, strides), "second conceal emits");
        check(luma_at(planes, strides, 59, 52) == square_luma, "square advanced further on the 2nd conceal");

    }

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;

}
