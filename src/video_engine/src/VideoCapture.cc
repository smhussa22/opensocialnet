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

// project headers

namespace OpenSocialNet::Video
{

    bool VideoCapture::init(const char* device_path, int width, int height, int framerate) noexcept
    {

        device_fd = ::open(device_path, O_RDWR);

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

        // ask for YUYV: every UVC camera supports it; we convert to planar I420 on capture
        ::v4l2_format format { };
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = width;
        format.fmt.pix.height = height;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        format.fmt.pix.field = V4L2_FIELD_NONE;

        if (::ioctl(device_fd, VIDIOC_S_FMT, &format) == -1)
        {

            shutdown();
            return false;

        }

        // refuse if the driver picked a format we don't know how to handle
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV)
        {

            shutdown();
            return false;

        }

        // store what the driver actually set
        res_width = format.fmt.pix.width;
        res_height = format.fmt.pix.height;

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

        // close device and reset state
        if (device_fd != -1)
        {

            ::close(device_fd);
            device_fd = -1;

        }

        num_buffers = 0;
        res_width = 0;
        res_height = 0;

    }

    std::size_t VideoCapture::capture_frame(std::span<std::uint8_t> frame_buffer) noexcept
    {

        if (!valid()) return 0;

        // dequeue filled YUYV buffer, convert to planar I420, requeue
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

        // YUYV: each 4 bytes = [Y0 U Y1 V] for 2 horizontal pixels; I420: Y full-res, U/V at quarter-res
        const std::uint8_t* yuyv { buffers[buf.index] };
        std::uint8_t* y_dst { frame_buffer.data() };
        std::uint8_t* u_dst { frame_buffer.data() + y_plane_size };
        std::uint8_t* v_dst { frame_buffer.data() + y_plane_size + uv_plane_size };

        std::size_t yuyv_row_bytes { static_cast<std::size_t>(res_width) * 2 };
        for (std::size_t row { 0 }; row < static_cast<std::size_t>(res_height); ++row)
        {

            const std::uint8_t* row_in { yuyv + row * yuyv_row_bytes };
            std::uint8_t* row_y { y_dst + row * static_cast<std::size_t>(res_width) };

            for (std::size_t x { 0 }; x < static_cast<std::size_t>(res_width); ++x) row_y[x] = row_in[x * 2];

            // sample chroma from even rows only (4:2:0 vertical subsampling)
            if ((row & 1) == 0)
            {

                std::uint8_t* row_u { u_dst + (row / 2) * static_cast<std::size_t>(res_width / 2) };
                std::uint8_t* row_v { v_dst + (row / 2) * static_cast<std::size_t>(res_width / 2) };
                for (std::size_t x { 0 }; x < static_cast<std::size_t>(res_width / 2); ++x)
                {

                    row_u[x] = row_in[x * 4 + 1];
                    row_v[x] = row_in[x * 4 + 3];

                }

            }

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
