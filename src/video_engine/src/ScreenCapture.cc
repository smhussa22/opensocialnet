// related headers
#include "ScreenCapture.hh"

// c sys headers
#include <cstdio>

// cpp stdlib headers

// project headers

// X11 / MIT-SHM is Linux-only. On macOS/Windows we compile stubs that
// report "no screen capture" — the caller in network/main.cc treats
// init() returning false as "disable screenshare" and moves on.
#ifdef __linux__

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xutil.h>
extern "C"
{

#include <libavutil/pixfmt.h>

}

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

#elif defined(__APPLE__)

// macOS: FFmpeg's avfoundation "Capture screen N" input device. Same
// non-blocking read pattern as the mac VideoCapture — avfoundation runs its
// own capture thread, AVFMT_FLAG_NONBLOCK makes av_read_frame return
// AVERROR(EAGAIN) when its queue is empty instead of blocking the tick loop.

extern "C"
{

#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

}

namespace OpenSocialNet::Video
{

    bool ScreenCapture::init(int requested_width, int requested_height, unsigned long) noexcept
    {

        ::avdevice_register_all();

        const AVInputFormat* fmt { ::av_find_input_format("avfoundation") };
        if (!fmt)
        {

            std::printf("[scr-cap] avfoundation format not available — install Homebrew ffmpeg\n");
            return false;

        }

        // I420 needs even dimensions
        out_width = requested_width & ~1;
        out_height = requested_height & ~1;

        AVDictionary* opts { nullptr };
        ::av_dict_set(&opts, "framerate",      "30", 0);
        ::av_dict_set(&opts, "capture_cursor", "1",  0);

        AVFormatContext* ctx { nullptr };
        const int ret { ::avformat_open_input(&ctx, "Capture screen 0:none", const_cast<AVInputFormat*>(fmt), &opts) };
        ::av_dict_free(&opts);

        if (ret < 0)
        {

            char errbuf[256] { };
            ::av_strerror(ret, errbuf, sizeof errbuf);
            std::printf("[scr-cap] avfoundation open 'Capture screen 0': %s — grant Screen Recording permission in System Settings\n", errbuf);
            shutdown();
            return false;

        }

        ctx->flags |= AVFMT_FLAG_NONBLOCK;
        av_fmt_ctx_ = ctx;

        if (::avformat_find_stream_info(ctx, nullptr) < 0)
        {

            std::printf("[scr-cap] avfoundation: find_stream_info failed\n");
            shutdown();
            return false;

        }

        for (unsigned i { 0 }; i < ctx->nb_streams; ++i)
        {

            if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { video_stream_idx_ = static_cast<int>(i); break; }

        }
        if (video_stream_idx_ < 0) { std::printf("[scr-cap] avfoundation: no video stream\n"); shutdown(); return false; }

        const AVCodecParameters* par { ctx->streams[video_stream_idx_]->codecpar };
        const AVCodec* codec { ::avcodec_find_decoder(par->codec_id) };
        if (!codec) { shutdown(); return false; }

        AVCodecContext* cctx { ::avcodec_alloc_context3(codec) };
        if (!cctx) { shutdown(); return false; }
        av_codec_ctx_ = cctx;

        ::avcodec_parameters_to_context(cctx, par);
        if (::avcodec_open2(cctx, codec, nullptr) < 0) { shutdown(); return false; }

        src_width = cctx->width;
        src_height = cctx->height;

        av_sws_ctx_ = ::sws_getContext(src_width, src_height, cctx->pix_fmt, out_width, out_height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!av_sws_ctx_) { shutdown(); return false; }

        av_packet_ = ::av_packet_alloc();
        av_frame_ = ::av_frame_alloc();
        if (!av_packet_ or !av_frame_) { shutdown(); return false; }

        std::printf("[scr-cap] grabbing display 0 %dx%d -> %dx%d\n", src_width, src_height, out_width, out_height);
        return true;

    }

    void ScreenCapture::shutdown() noexcept
    {

        if (av_packet_)    { auto* p { static_cast<AVPacket*>(av_packet_) };          ::av_packet_free(&p);          av_packet_    = nullptr; }
        if (av_frame_)     { auto* f { static_cast<AVFrame*>(av_frame_) };            ::av_frame_free(&f);           av_frame_     = nullptr; }
        if (av_sws_ctx_)   { ::sws_freeContext(static_cast<SwsContext*>(av_sws_ctx_)); av_sws_ctx_ = nullptr; }
        if (av_codec_ctx_) { auto* c { static_cast<AVCodecContext*>(av_codec_ctx_) }; ::avcodec_free_context(&c);    av_codec_ctx_ = nullptr; }
        if (av_fmt_ctx_)   { auto* c { static_cast<AVFormatContext*>(av_fmt_ctx_) };  ::avformat_close_input(&c);    av_fmt_ctx_   = nullptr; }
        video_stream_idx_ = -1;
        src_width = 0;
        src_height = 0;
        out_width = 0;
        out_height = 0;

    }

    std::size_t ScreenCapture::capture_frame(std::span<std::uint8_t> frame_buffer) noexcept
    {

        if (!valid()) return 0;

        auto* ctx { static_cast<AVFormatContext*>(av_fmt_ctx_) };
        auto* cctx { static_cast<AVCodecContext*>(av_codec_ctx_) };
        auto* swsc { static_cast<SwsContext*>(av_sws_ctx_) };
        auto* pkt { static_cast<AVPacket*>(av_packet_) };
        auto* frame { static_cast<AVFrame*>(av_frame_) };

        const std::size_t y_plane_size { static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height) };
        const std::size_t i420_size { y_plane_size * 3 / 2 };
        if (frame_buffer.size() < i420_size) return 0;

        // The device captures at 30fps but we're only polled at the (lower)
        // screenshare fps — drain everything queued and keep the newest frame
        // so latency doesn't build up in avfoundation's internal queue.
        bool got { false };
        while (true)
        {

            const int ret { ::av_read_frame(ctx, pkt) };
            if (ret < 0) break; // EAGAIN = queue drained
            if (pkt->stream_index == video_stream_idx_ and ::avcodec_send_packet(cctx, pkt) >= 0 and ::avcodec_receive_frame(cctx, frame) >= 0) got = true;
            ::av_packet_unref(pkt);

        }
        if (!got) return 0;

        std::uint8_t* dst_planes[4] { frame_buffer.data(), frame_buffer.data() + y_plane_size, frame_buffer.data() + y_plane_size + y_plane_size / 4, nullptr };
        const int dst_strides[4] { out_width, out_width / 2, out_width / 2, 0 };
        ::sws_scale(swsc, frame->data, frame->linesize, 0, src_height, dst_planes, dst_strides);

        return i420_size;

    }

    bool ScreenCapture::valid()  const noexcept { return av_fmt_ctx_ != nullptr and av_sws_ctx_ != nullptr; }
    int  ScreenCapture::width()  const noexcept { return out_width;  }
    int  ScreenCapture::height() const noexcept { return out_height; }

}

#else // other platforms — screenshare unsupported

namespace OpenSocialNet::Video
{

    bool ScreenCapture::init(int, int, unsigned long) noexcept
    {

        std::printf("[scr-cap] screenshare not supported on this platform — screenshare disabled\n");
        return false;

    }

    void ScreenCapture::shutdown() noexcept { }

    std::size_t ScreenCapture::capture_frame(std::span<std::uint8_t>) noexcept { return 0; }

    bool ScreenCapture::valid()  const noexcept { return false; }
    int  ScreenCapture::width()  const noexcept { return out_width;  }
    int  ScreenCapture::height() const noexcept { return out_height; }

}

#endif // __linux__ / __APPLE__
