// related headers
#include "ScreenCapture.hh"

// c sys headers
#include <cstdio>
#include <sys/ipc.h>
#include <sys/shm.h>

// cpp stdlib headers

// 3rd party headers
#include <X11/Xutil.h>
extern "C"
{

#include <libavutil/pixfmt.h>

}

// project headers

namespace
{

    // A grab target can vanish between the liveness check and the
    // XShmGetImage (window closed mid-frame) — without a handler Xlib
    // would abort the whole client on the resulting BadDrawable.
    int ignore_x_error(::Display*, ::XErrorEvent*) noexcept
    {

        return 0;

    }

}

namespace OpenSocialNet::Video
{

    bool ScreenCapture::init(int requested_width, int requested_height, unsigned long window_id) noexcept
    {

        display.reset(::XOpenDisplay(nullptr));
        if (!display)
        {

            std::printf("[scr-cap] XOpenDisplay failed — is $DISPLAY set?\n");
            return false;

        }

        const int screen { ::XDefaultScreen(display.get()) };
        root = ::XRootWindow(display.get(), screen);

        if (!::XShmQueryExtension(display.get()))
        {

            std::printf("[scr-cap] X server lacks the MIT-SHM extension\n");
            shutdown();
            return false;

        }

        ::XSetErrorHandler(ignore_x_error);

        // I420 needs even dimensions
        out_width = requested_width & ~1;
        out_height = requested_height & ~1;

        // explicit id wins; otherwise try to latch onto a window now and
        // keep grabbing the (black-on-WSLg) root until one shows up
        auto_pick = window_id == 0;
        ::Window wanted { auto_pick ? pick_largest_window() : static_cast<::Window>(window_id) };
        if (wanted == 0) wanted = root;

        if (!setup_grab(wanted) and (!auto_pick or wanted == root or !setup_grab(root)))
        {

            shutdown();
            return false;

        }

        if (target == root) std::printf("[scr-cap] grabbing root %dx%d%s\n", src_width, src_height, auto_pick ? " — will latch onto the largest window once one appears" : "");
        else std::printf("[scr-cap] grabbing window 0x%lx %dx%d\n", static_cast<unsigned long>(target), src_width, src_height);

        return true;

    }

    bool ScreenCapture::setup_grab(::Window window) noexcept
    {

        ::XWindowAttributes attrs { };
        if (!::XGetWindowAttributes(display.get(), window, &attrs)) return false;
        if (attrs.width <= 0 or attrs.height <= 0) return false;

        teardown_grab();

        shm_image = ::XShmCreateImage(display.get(), attrs.visual, static_cast<unsigned int>(attrs.depth), ZPixmap, nullptr, &shm_info, static_cast<unsigned int>(attrs.width), static_cast<unsigned int>(attrs.height));
        if (!shm_image)
        {

            std::printf("[scr-cap] XShmCreateImage failed\n");
            return false;

        }

        // the scaler below assumes 32bpp little-endian BGRX (BGR0)
        if (shm_image->bits_per_pixel != 32)
        {

            std::printf("[scr-cap] unsupported pixel layout: %d bpp\n", shm_image->bits_per_pixel);
            teardown_grab();
            return false;

        }

        shm_info.shmid = ::shmget(IPC_PRIVATE, static_cast<std::size_t>(shm_image->bytes_per_line) * static_cast<std::size_t>(shm_image->height), IPC_CREAT | 0600);
        if (shm_info.shmid == -1)
        {

            std::printf("[scr-cap] shmget failed\n");
            teardown_grab();
            return false;

        }

        shm_info.shmaddr = static_cast<char*>(::shmat(shm_info.shmid, nullptr, 0));
        if (shm_info.shmaddr == reinterpret_cast<char*>(-1))
        {

            shm_info.shmaddr = nullptr;
            std::printf("[scr-cap] shmat failed\n");
            teardown_grab();
            return false;

        }
        shm_image->data = shm_info.shmaddr;
        shm_info.readOnly = False;

        if (!::XShmAttach(display.get(), &shm_info))
        {

            std::printf("[scr-cap] XShmAttach failed\n");
            teardown_grab();
            return false;

        }
        shm_attached = true;
        ::XSync(display.get(), False);

        // mark for removal now — the kernel frees the segment once both we
        // and the X server detach, even if this process crashes.
        ::shmctl(shm_info.shmid, IPC_RMID, nullptr);

        sws.reset(::sws_getContext(attrs.width, attrs.height, AV_PIX_FMT_BGR0, out_width, out_height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws)
        {

            std::printf("[scr-cap] sws_getContext failed (%dx%d -> %dx%d)\n", attrs.width, attrs.height, out_width, out_height);
            teardown_grab();
            return false;

        }

        target = window;
        src_width = attrs.width;
        src_height = attrs.height;
        return true;

    }

    void ScreenCapture::teardown_grab() noexcept
    {

        if (shm_attached)
        {

            ::XShmDetach(display.get(), &shm_info);
            ::XSync(display.get(), False);
            shm_attached = false;

        }

        // XDestroyImage (a macro, so no :: prefix) would free() image->data
        // — it's shm memory, so null it first and detach the segment
        // ourselves.
        if (shm_image != nullptr)
        {

            shm_image->data = nullptr;
            XDestroyImage(shm_image);
            shm_image = nullptr;

        }

        if (shm_info.shmaddr != nullptr)
        {

            ::shmdt(shm_info.shmaddr);
            shm_info.shmaddr = nullptr;

        }

        sws.reset();
        src_width = 0;
        src_height = 0;

    }

    ::Window ScreenCapture::pick_largest_window() const noexcept
    {

        ::Window root_return { 0 };
        ::Window parent_return { 0 };
        ::Window* children { nullptr };
        unsigned int child_count { 0 };
        if (!::XQueryTree(display.get(), root, &root_return, &parent_return, &children, &child_count)) return 0;

        ::Window best { 0 };
        long best_area { 0 };
        for (unsigned int i { 0 }; i < child_count; ++i)
        {

            ::XWindowAttributes attrs { };
            if (!::XGetWindowAttributes(display.get(), children[i], &attrs)) continue;
            if (attrs.map_state != IsViewable or attrs.depth < 24) continue;
            if (attrs.width < 64 or attrs.height < 64) continue; // skip tray / utility specks

            const long area { static_cast<long>(attrs.width) * attrs.height };
            if (area > best_area)
            {

                best_area = area;
                best = children[i];

            }

        }

        if (children != nullptr) ::XFree(children);
        return best;

    }

    void ScreenCapture::shutdown() noexcept
    {

        if (display) teardown_grab();

        display.reset();
        root = 0;
        target = 0;
        out_width = 0;
        out_height = 0;

    }

    std::size_t ScreenCapture::capture_frame(std::span<std::uint8_t> frame_buffer) noexcept
    {

        if (!valid()) return 0;

        // auto mode parked on the root: keep looking for a real window
        if (auto_pick and target == root)
        {

            ::Window found { pick_largest_window() };
            if (found != 0 and setup_grab(found)) std::printf("[scr-cap] latched onto window 0x%lx %dx%d\n", static_cast<unsigned long>(target), src_width, src_height);
            if (!valid()) { setup_grab(root); return 0; }

        }

        // target may have resized or died since the last grab
        if (target != root)
        {

            ::XWindowAttributes attrs { };
            if (!::XGetWindowAttributes(display.get(), target, &attrs) or attrs.map_state != IsViewable)
            {

                std::printf("[scr-cap] window 0x%lx gone — back to %s\n", static_cast<unsigned long>(target), auto_pick ? "auto-pick" : "root");
                setup_grab(root);
                return 0;

            }
            if ((attrs.width != src_width or attrs.height != src_height) and !setup_grab(target))
            {

                setup_grab(root);
                return 0;

            }

        }

        const std::size_t y_plane_size { static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height) };
        const std::size_t uv_plane_size { static_cast<std::size_t>(out_width / 2) * static_cast<std::size_t>(out_height / 2) };
        const std::size_t i420_size { y_plane_size + 2 * uv_plane_size };
        if (frame_buffer.size() < i420_size) return 0;

        if (!::XShmGetImage(display.get(), target, shm_image, 0, 0, AllPlanes)) return 0;

        const std::uint8_t* src_planes[4] { reinterpret_cast<const std::uint8_t*>(shm_image->data), nullptr, nullptr, nullptr };
        const int src_strides[4] { shm_image->bytes_per_line, 0, 0, 0 };
        std::uint8_t* dst_planes[4] { frame_buffer.data(), frame_buffer.data() + y_plane_size, frame_buffer.data() + y_plane_size + uv_plane_size, nullptr };
        const int dst_strides[4] { out_width, out_width / 2, out_width / 2, 0 };

        ::sws_scale(sws.get(), src_planes, src_strides, 0, src_height, dst_planes, dst_strides);

        return i420_size;

    }

    bool ScreenCapture::valid() const noexcept
    {

        return display != nullptr and sws != nullptr;

    }

    int ScreenCapture::width() const noexcept
    {

        return out_width;

    }

    int ScreenCapture::height() const noexcept
    {

        return out_height;

    }

}
