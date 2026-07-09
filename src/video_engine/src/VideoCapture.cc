// related headers
#include "VideoCapture.hh"

// c sys headers
#include <cstddef>
#include <cstdint>
#include <cstdio>

// cpp stdlib headers
#include <span>
#include <string>

// project headers

// V4L2 is a Linux kernel API — the whole camera-capture implementation
// below is Linux-only. On macOS/Windows we compile a stub that just
// reports "no camera"; the caller (network/main.cc) already falls back
// to synthetic video in that case.
#ifdef __linux__

// c sys headers
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

// 3rd party headers
#include <linux/videodev2.h>
extern "C"
{

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

}

namespace
{

    // Init failures were previously silent, which made the WSL camera
    // flakiness (EACCES from stale group membership vs EBUSY from a
    // not-yet-released device) impossible to tell apart from the logs.
    void log_init_failure(const char* device_path, const char* step) noexcept
    {

        std::printf("[vid-cap] %s: %s failed: %s\n", device_path, step, std::strerror(errno));

    }

}

namespace OpenSocialNet::Video
{

    bool VideoCapture::init(const char* device_path, int width, int height, int framerate) noexcept
    {

        // O_NONBLOCK: DQBUF returns EAGAIN instead of stalling the caller
        // until the camera's next frame — the call client polls from its
        // 10ms main loop and must never block behind a 30fps exposure.
        device_fd = ::open(device_path, O_RDWR | O_NONBLOCK);
        if (device_fd == -1)
        {

            log_init_failure(device_path, "open");
            return false;

        }

        ::v4l2_capability cap { };
        int verify_status { ::ioctl(device_fd, VIDIOC_QUERYCAP, &cap) };
        if (verify_status == -1)
        {

            log_init_failure(device_path, "VIDIOC_QUERYCAP (not a v4l2 device?)");
            shutdown();
            return false;

        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        {

            std::printf("[vid-cap] %s: device lacks V4L2_CAP_VIDEO_CAPTURE\n", device_path);
            shutdown();
            return false;

        }

        // ask for MJPG: ~10x less USB-IP bandwidth than YUYV, decoded by libavcodec into I420
        ::v4l2_format format { };
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = width;
        format.fmt.pix.height = height;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        format.fmt.pix.field = V4L2_FIELD_NONE;

        if (::ioctl(device_fd, VIDIOC_S_FMT, &format) == -1)
        {

            log_init_failure(device_path, "VIDIOC_S_FMT");
            shutdown();
            return false;

        }

        // refuse if the driver picked a format we don't know how to handle
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG)
        {

            std::printf("[vid-cap] %s: driver refused MJPEG (gave fourcc %08x)\n", device_path, format.fmt.pix.pixelformat);
            shutdown();
            return false;

        }

        // store what the driver actually set
        res_width = format.fmt.pix.width;
        res_height = format.fmt.pix.height;

        // bring up the MJPEG decoder (libavcodec) — produces YUVJ420P which is byte-compatible with I420
        const ::AVCodec* codec { ::avcodec_find_decoder(AV_CODEC_ID_MJPEG) };
        if (!codec) { shutdown(); return false; }

        ::AVCodecContext* ctx { ::avcodec_alloc_context3(codec) };
        if (!ctx) { shutdown(); return false; }
        mjpeg_ctx.reset(ctx);

        if (::avcodec_open2(mjpeg_ctx.get(), codec, nullptr) < 0) { shutdown(); return false; }

        ::AVPacket* pkt { ::av_packet_alloc() };
        if (!pkt) { shutdown(); return false; }
        mjpeg_packet.reset(pkt);

        ::AVFrame* fr { ::av_frame_alloc() };
        if (!fr) { shutdown(); return false; }
        mjpeg_frame.reset(fr);

        ::v4l2_streamparm parm { };
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = framerate;
        ::ioctl(device_fd, VIDIOC_S_PARM, &parm); // non-fatal if fails

        // request and map mmap buffers
        ::v4l2_requestbuffers req { };
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(device_fd, VIDIOC_REQBUFS, &req) == -1)
        {

            log_init_failure(device_path, "VIDIOC_REQBUFS (device busy / previous instance not released?)");
            shutdown();
            return false;

        }
        num_buffers = req.count;

        for (std::size_t i { 0 }; i < num_buffers; ++i)
        {

            ::v4l2_buffer buf { };
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (::ioctl(device_fd, VIDIOC_QUERYBUF, &buf) == -1)
            {

                log_init_failure(device_path, "VIDIOC_QUERYBUF");
                shutdown();
                return false;

            }

            buffer_lengths[i] = buf.length;
            buffers[i] = static_cast<std::uint8_t*>(::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd, buf.m.offset));
            if (buffers[i] == MAP_FAILED)
            {

                log_init_failure(device_path, "mmap");
                buffers[i] = nullptr;
                shutdown();
                return false;

            }

        }

        // queue all buffers then start streaming
        for (std::size_t i { 0 }; i < num_buffers; ++i)
        {

            ::v4l2_buffer buf { };
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (::ioctl(device_fd, VIDIOC_QBUF, &buf) == -1)
            {

                log_init_failure(device_path, "VIDIOC_QBUF");
                shutdown();
                return false;

            }

        }

        ::v4l2_buf_type type { V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (::ioctl(device_fd, VIDIOC_STREAMON, &type) == -1)
        {

            log_init_failure(device_path, "VIDIOC_STREAMON");
            shutdown();
            return false;

        }

        return true;

    }

    void VideoCapture::shutdown() noexcept
    {

        // stop streaming and unmap buffers
        if (device_fd != -1)
        {

            ::v4l2_buf_type type { V4L2_BUF_TYPE_VIDEO_CAPTURE };
            ::ioctl(device_fd, VIDIOC_STREAMOFF, &type);

        }

        for (std::size_t i { 0 }; i < num_buffers; ++i)
        {

            if (buffers[i] != nullptr)
            {

                ::munmap(buffers[i], buffer_lengths[i]);
                buffers[i] = nullptr;

            }

        }

        // close device, drop MJPEG decoder, and reset state
        if (device_fd != -1)
        {

            ::close(device_fd);
            device_fd = -1;

        }

        mjpeg_frame.reset();
        mjpeg_packet.reset();
        mjpeg_ctx.reset();

        num_buffers = 0;
        res_width = 0;
        res_height = 0;

    }

    std::size_t VideoCapture::capture_frame(std::span<std::uint8_t> frame_buffer) noexcept
    {

        if (!valid()) return 0;

        // dequeue filled MJPEG buffer, decode to I420, requeue
        ::v4l2_buffer buf { };
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(device_fd, VIDIOC_DQBUF, &buf) == -1) return 0;

        std::size_t y_plane_size { static_cast<std::size_t>(res_width) * static_cast<std::size_t>(res_height) };
        std::size_t uv_plane_size { static_cast<std::size_t>(res_width / 2) * static_cast<std::size_t>(res_height / 2) };
        std::size_t i420_size { y_plane_size + 2 * uv_plane_size };

        if (frame_buffer.size() < i420_size)
        {

            ::ioctl(device_fd, VIDIOC_QBUF, &buf);
            return 0;

        }

        // feed the mmap'd JPEG bytes through libavcodec; output is YUVJ420P / YUV420P (byte-identical layout to I420)
        mjpeg_packet->data = buffers[buf.index];
        mjpeg_packet->size = static_cast<int>(buf.bytesused);

        int send_result { ::avcodec_send_packet(mjpeg_ctx.get(), mjpeg_packet.get()) };
        if (send_result < 0)
        {

            ::ioctl(device_fd, VIDIOC_QBUF, &buf);
            return 0;

        }

        int recv_result { ::avcodec_receive_frame(mjpeg_ctx.get(), mjpeg_frame.get()) };
        if (recv_result < 0)
        {

            ::ioctl(device_fd, VIDIOC_QBUF, &buf);
            return 0;

        }

        // copy each plane row-by-row to strip the decoder's internal padding (linesize >= width)
        std::uint8_t* y_dst { frame_buffer.data() };
        std::uint8_t* u_dst { frame_buffer.data() + y_plane_size };
        std::uint8_t* v_dst { frame_buffer.data() + y_plane_size + uv_plane_size };

        for (std::size_t row { 0 }; row < static_cast<std::size_t>(res_height); ++row)
        {

            std::memcpy(y_dst + row * static_cast<std::size_t>(res_width), mjpeg_frame->data[0] + row * static_cast<std::size_t>(mjpeg_frame->linesize[0]), static_cast<std::size_t>(res_width));

        }

        for (std::size_t row { 0 }; row < static_cast<std::size_t>(res_height / 2); ++row)
        {

            std::memcpy(u_dst + row * static_cast<std::size_t>(res_width / 2), mjpeg_frame->data[1] + row * static_cast<std::size_t>(mjpeg_frame->linesize[1]), static_cast<std::size_t>(res_width / 2));
            std::memcpy(v_dst + row * static_cast<std::size_t>(res_width / 2), mjpeg_frame->data[2] + row * static_cast<std::size_t>(mjpeg_frame->linesize[2]), static_cast<std::size_t>(res_width / 2));

        }

        if (::ioctl(device_fd, VIDIOC_QBUF, &buf) == -1) return 0;

        return i420_size;

    }

    bool VideoCapture::valid() const noexcept
    {

        return device_fd != -1;

    }

    int VideoCapture::width() const noexcept
    {

        return res_width;

    }

    int VideoCapture::height() const noexcept
    {

        return res_height;

    }

}

#elif defined(__APPLE__)

// macOS: capture via FFmpeg's avfoundation input device.
// avfoundation feeds an internal capture thread so av_read_frame returns from
// an internal queue; AVFMT_FLAG_NONBLOCK makes it return AVERROR(EAGAIN) when
// the queue is empty instead of blocking, keeping capture_frame() latency low.

#include <cstring>

extern "C"
{

#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

}

namespace OpenSocialNet::Video
{

    bool VideoCapture::init(const char* device_path, int width, int height, int framerate) noexcept
    {

        avdevice_register_all();

        const AVInputFormat* fmt { av_find_input_format("avfoundation") };
        if (!fmt)
        {
            std::printf("[vid-cap] avfoundation format not available — install Homebrew ffmpeg\n");
            return false;
        }

        // avfoundation device string: "<video_idx>:<audio_idx>" — pass "none" for audio
        const std::string avf_dev { std::string(device_path) + ":none" };

        char fps_str[16] { };
        std::snprintf(fps_str, sizeof fps_str, "%d", framerate);
        char sz_str[32] { };
        std::snprintf(sz_str,  sizeof sz_str,  "%dx%d", width, height);

        AVDictionary* opts { nullptr };
        ::av_dict_set(&opts, "framerate",   fps_str, 0);
        ::av_dict_set(&opts, "video_size",  sz_str,  0);

        AVFormatContext* ctx { nullptr };
        const int ret { ::avformat_open_input(&ctx, avf_dev.c_str(), const_cast<AVInputFormat*>(fmt), &opts) };
        ::av_dict_free(&opts);

        if (ret < 0)
        {
            char errbuf[256] { };
            ::av_strerror(ret, errbuf, sizeof errbuf);
            std::printf("[vid-cap] avfoundation open '%s': %s\n", avf_dev.c_str(), errbuf);
            return false;
        }

        ctx->flags |= AVFMT_FLAG_NONBLOCK;
        av_fmt_ctx_ = ctx;

        if (::avformat_find_stream_info(ctx, nullptr) < 0)
        {
            std::printf("[vid-cap] avfoundation: find_stream_info failed\n");
            shutdown();
            return false;
        }

        for (unsigned i { 0 }; i < ctx->nb_streams; ++i)
        {
            if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                video_stream_idx_ = static_cast<int>(i);
                break;
            }
        }
        if (video_stream_idx_ < 0) { std::printf("[vid-cap] avfoundation: no video stream\n"); shutdown(); return false; }

        const AVCodecParameters* par { ctx->streams[video_stream_idx_]->codecpar };
        const AVCodec* codec { ::avcodec_find_decoder(par->codec_id) };
        if (!codec) { shutdown(); return false; }

        AVCodecContext* cctx { ::avcodec_alloc_context3(codec) };
        if (!cctx) { shutdown(); return false; }
        av_codec_ctx_ = cctx;

        ::avcodec_parameters_to_context(cctx, par);
        if (::avcodec_open2(cctx, codec, nullptr) < 0) { shutdown(); return false; }

        res_width  = cctx->width;
        res_height = cctx->height;

        av_sws_ctx_ = ::sws_getContext(res_width, res_height, cctx->pix_fmt,
                                       res_width, res_height, AV_PIX_FMT_YUV420P,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!av_sws_ctx_) { shutdown(); return false; }

        av_packet_ = ::av_packet_alloc();
        av_frame_  = ::av_frame_alloc();
        if (!av_packet_ or !av_frame_) { shutdown(); return false; }

        return true;

    }

    void VideoCapture::shutdown() noexcept
    {

        if (av_packet_)   { auto* p { static_cast<AVPacket*>(av_packet_) };         ::av_packet_free(&p);        av_packet_   = nullptr; }
        if (av_frame_)    { auto* f { static_cast<AVFrame*>(av_frame_) };            ::av_frame_free(&f);         av_frame_    = nullptr; }
        if (av_sws_ctx_)  { ::sws_freeContext(static_cast<SwsContext*>(av_sws_ctx_)); av_sws_ctx_ = nullptr; }
        if (av_codec_ctx_){ auto* c { static_cast<AVCodecContext*>(av_codec_ctx_) }; ::avcodec_free_context(&c);  av_codec_ctx_ = nullptr; }
        if (av_fmt_ctx_)  { auto* c { static_cast<AVFormatContext*>(av_fmt_ctx_) };  ::avformat_close_input(&c);  av_fmt_ctx_  = nullptr; }
        video_stream_idx_ = -1;
        res_width  = 0;
        res_height = 0;

    }

    std::size_t VideoCapture::capture_frame(std::span<std::uint8_t> frame_buffer) noexcept
    {

        if (!valid()) return 0;

        auto* ctx   { static_cast<AVFormatContext*>(av_fmt_ctx_) };
        auto* cctx  { static_cast<AVCodecContext*>(av_codec_ctx_) };
        auto* swsc  { static_cast<SwsContext*>(av_sws_ctx_) };
        auto* pkt   { static_cast<AVPacket*>(av_packet_) };
        auto* frame { static_cast<AVFrame*>(av_frame_) };

        const std::size_t yuv_size { static_cast<std::size_t>(res_width) * static_cast<std::size_t>(res_height) * 3u / 2u };
        if (frame_buffer.size() < yuv_size) return 0;

        while (true)
        {

            const int ret { ::av_read_frame(ctx, pkt) };
            if (ret == AVERROR(EAGAIN) or ret < 0) return 0;
            if (pkt->stream_index != video_stream_idx_) { ::av_packet_unref(pkt); continue; }

            bool got { false };
            if (::avcodec_send_packet(cctx, pkt) >= 0 and ::avcodec_receive_frame(cctx, frame) >= 0)
            {

                std::uint8_t* dst[4] {
                    frame_buffer.data(),
                    frame_buffer.data() + static_cast<std::size_t>(res_width) * static_cast<std::size_t>(res_height),
                    frame_buffer.data() + static_cast<std::size_t>(res_width) * static_cast<std::size_t>(res_height) * 5u / 4u,
                    nullptr
                };
                int dst_stride[4] { res_width, res_width / 2, res_width / 2, 0 };
                ::sws_scale(swsc, frame->data, frame->linesize, 0, res_height, dst, dst_stride);
                got = true;

            }
            ::av_packet_unref(pkt);
            if (got) return yuv_size;

        }

    }

    bool VideoCapture::valid()  const noexcept { return av_fmt_ctx_ != nullptr; }
    int  VideoCapture::width()  const noexcept { return res_width;  }
    int  VideoCapture::height() const noexcept { return res_height; }

}

#else // non-Linux, non-macOS stubs

namespace OpenSocialNet::Video
{

    bool VideoCapture::init(const char*, int, int, int) noexcept
    {

        std::printf("[vid-cap] camera capture not supported on this platform\n");
        return false;

    }

    void VideoCapture::shutdown() noexcept { }

    std::size_t VideoCapture::capture_frame(std::span<std::uint8_t>) noexcept { return 0; }

    bool VideoCapture::valid()  const noexcept { return false; }
    int  VideoCapture::width()  const noexcept { return res_width;  }
    int  VideoCapture::height() const noexcept { return res_height; }

}

#endif // __linux__ / __APPLE__
