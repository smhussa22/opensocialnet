// probe: list pixel formats and supported (size, fps) combinations for /dev/video0

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static void print_fourcc(std::uint32_t f)
{

    char s[5] { static_cast<char>(f & 0xff), static_cast<char>((f >> 8) & 0xff), static_cast<char>((f >> 16) & 0xff), static_cast<char>((f >> 24) & 0xff), 0 };
    ::printf("%s", s);

}

static void enumerate_intervals(int fd, std::uint32_t pixfmt, std::uint32_t w, std::uint32_t h)
{

    ::printf("        intervals:");
    for (std::uint32_t k { 0 }; ; ++k)
    {

        ::v4l2_frmivalenum iv { };
        iv.index = k;
        iv.pixel_format = pixfmt;
        iv.width = w;
        iv.height = h;
        if (::ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) == -1) break;
        if (iv.type == V4L2_FRMIVAL_TYPE_DISCRETE)
        {

            double fps { static_cast<double>(iv.discrete.denominator) / static_cast<double>(iv.discrete.numerator) };
            ::printf("  %.1ffps", fps);

        }

    }
    ::printf("\n");

}

int main()
{

    int fd { ::open("/dev/video0", O_RDWR) };
    if (fd < 0) { ::printf("open failed\n"); return 1; }

    for (std::uint32_t i { 0 }; ; ++i)
    {

        ::v4l2_fmtdesc d { };
        d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        d.index = i;
        if (::ioctl(fd, VIDIOC_ENUM_FMT, &d) == -1) break;
        ::printf("[%u] ", i);
        print_fourcc(d.pixelformat);
        ::printf("  %s\n", d.description);

        for (std::uint32_t j { 0 }; ; ++j)
        {

            ::v4l2_frmsizeenum fs { };
            fs.index = j;
            fs.pixel_format = d.pixelformat;
            if (::ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == -1) break;
            if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE)
            {

                ::printf("    %ux%u\n", fs.discrete.width, fs.discrete.height);
                enumerate_intervals(fd, d.pixelformat, fs.discrete.width, fs.discrete.height);

            }

        }

    }

    ::close(fd);
    return 0;

}
