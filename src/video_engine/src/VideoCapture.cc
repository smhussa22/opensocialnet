// related headers
#include "VideoCapture.hh"

// c sys headers
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <cstring>

// cpp stdlib headers
#include <span>

// 3rd party headers
#include <linux/videodev2.h>
extern "C"
{

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

}

// project headers

namespace OpenSocialNet::Video
{

    bool VideoCapture::init(const char* device_path, int width, int height, int framerate) noexcept
    {

        // O_NONBLOCK: DQBUF returns EAGAIN instead of stalling the caller
        // until the camera's next frame — the call client polls from its
        // 10ms main loop and must never block behind a 30fps exposure.
        device_fd = ::open(device_path, O_RDWR | O_NONBLOCK);

        ::v4l2_capability cap { };
        int verify_status { ::ioctl(device_fd, VIDIOC_QUERYCAP, &cap) };
        if (verify_status == -1)
        {

            shutdown();
            return false;

        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
        {

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

            shutdown();
            return false;

        }

        // refuse if the driver picked a format we don't know how to handle
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG)
        {

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

                shutdown();
                return false;

            }

            buffer_lengths[i] = buf.length;
            buffers[i] = static_cast<std::uint8_t*>(::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd, buf.m.offset));
            if (buffers[i] == MAP_FAILED)
            {

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

                shutdown();
                return false;

            }

        }

        ::v4l2_buf_type type { V4L2_BUF_TYPE_VIDEO_CAPTURE };
        if (::ioctl(device_fd, VIDIOC_STREAMON, &type) == -1)
        {

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
