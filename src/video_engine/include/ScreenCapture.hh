#ifndef SCREEN_CAPTURE_HH
#define SCREEN_CAPTURE_HH

// related headers

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <span>

// 3rd party headers
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

// project headers
#include "ScreenCaptureDeleters.hh"

namespace OpenSocialNet::Video
{

    // Grabs an X11 window through the MIT-SHM extension — one
    // shared-memory XImage reused every frame, no per-frame allocation or
    // socket copy — then scales + converts BGR0 -> I420 with libswscale
    // so the output plugs straight into the same VideoEncoder lane the
    // camera uses.
    //
    // Target selection: an explicit window id, or auto mode (id 0) which
    // latches onto the largest viewable top-level window. On WSLg the
    // root window is always black (XWayland is rootless — nothing ever
    // paints into it), so auto mode keeps re-checking until a real window
    // appears, and falls back to the root again if the target dies.
    class ScreenCapture
    {

    public:
        ScreenCapture() noexcept = default;
        ~ScreenCapture() { shutdown(); }

        ScreenCapture(const ScreenCapture&) = delete;
        ScreenCapture& operator=(const ScreenCapture&) = delete;
        ScreenCapture(ScreenCapture&&) = delete;
        ScreenCapture& operator=(ScreenCapture&&) = delete;

        // Connects to $DISPLAY, picks the grab target (explicit window_id,
        // or 0 = auto), and builds the scaler to out_width x out_height
        // (forced even for I420). Returns true on success.
        bool init(int out_width, int out_height, unsigned long window_id = 0) noexcept;

        // Detaches the shm segment, drops the scaler, closes the display.
        void shutdown() noexcept;

        // Grabs the screen and writes one I420 frame at the configured
        // output size. Returns frame size in bytes, 0 on failure.
        std::size_t capture_frame(std::span<std::uint8_t> frame_buffer) noexcept;

        // returns whether the grab pipeline is up.
        [[nodiscard]] bool valid() const noexcept;

        // returns output frame width/height.
        [[nodiscard]] int width() const noexcept;
        [[nodiscard]] int height() const noexcept;

    private:
        // Tears down + rebuilds the shm image and scaler for a new grab
        // target (also called on resize). Returns true on success.
        bool setup_grab(::Window window) noexcept;

        // Drops the shm image / segment / scaler but keeps the display.
        void teardown_grab() noexcept;

        // Largest viewable top-level window, or 0 if none exists yet.
        [[nodiscard]] ::Window pick_largest_window() const noexcept;


        X11DisplayPtr display { }; // connection to the X server
        ::Window root { 0 }; // root window (fallback grab target)
        ::Window target { 0 }; // window currently being grabbed
        bool auto_pick { true }; // no explicit id given — latch onto the largest window
        ::XImage* shm_image { nullptr }; // shared-memory image reused every grab
        ::XShmSegmentInfo shm_info { }; // shm segment backing shm_image
        bool shm_attached { false }; // segment attached to the X server
        SwsContextPtr sws { }; // BGR0 target-sized -> I420 output scaler
        int src_width { 0 }; // grab target width
        int src_height { 0 }; // grab target height
        int out_width { 0 }; // output frame width (even)
        int out_height { 0 }; // output frame height (even)

    };

}

#endif // SCREEN_CAPTURE_HH
