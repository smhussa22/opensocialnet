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

        ::v4l2_format format { };
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = width;
        format.fmt.pix.height = height;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
        format.fmt.pix.field = V4L2_FIELD_NONE;

        if (::ioctl(device_fd, VIDIOC_S_FMT, &format) == -1)
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

        // dequeue filled buffer, copy data out, requeue for refill
        ::v4l2_buffer buf { };
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (::ioctl(device_fd, VIDIOC_DQBUF, &buf) == -1) return 0;

        std::size_t bytes_to_copy { std::min(frame_buffer.size(), static_cast<std::size_t>(buf.bytesused)) };
        std::memcpy(frame_buffer.data(), buffers[buf.index], bytes_to_copy);

        if (::ioctl(device_fd, VIDIOC_QBUF, &buf) == -1) return 0;

        return bytes_to_copy;

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
