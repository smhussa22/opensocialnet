#ifndef VIDEO_CAPTURE_HH
#define VIDEO_CAPTURE_HH

// c sys headers
#include <cstddef>

// cpp stdlib headers
#include <span>
#include <cstdint>

// 3rd party headers
#include <linux/videodev2.h>

// project headers
#include "VideoDecoderDeleter.hh"

namespace OpenSocialNet::Video
{

    class VideoCapture
    {

    public:
        VideoCapture() noexcept = default;
        ~VideoCapture() { shutdown(); }

        VideoCapture(const VideoCapture&) = delete;
        VideoCapture& operator=(const VideoCapture&) = delete;
        VideoCapture(VideoCapture&&) = delete;
        VideoCapture& operator=(VideoCapture&&) = delete;

        // opens video device, verifies its a capture device, and starts streaming from camera. returns true on success.
        bool init(const char* device_path, int width, int height, int framerate) noexcept;

        // stops capture and closes video device.
        void shutdown() noexcept;

        // reads next captured frame as YUV420P data. returns frame size in bytes.
        std::size_t capture_frame(std::span<std::uint8_t> frame_buffer) noexcept;

        // returns whether capture device was successfully initialized.
        [[nodiscard]] bool valid() const noexcept;

        // returns frame width/height.
        [[nodiscard]] int width() const noexcept;
        [[nodiscard]] int height() const noexcept;

    private:
        int device_fd { -1 }; // v4l2 device file descriptor
        std::uint8_t* buffers[4] { }; // mmap'd device buffers
        std::size_t buffer_lengths[4] { }; // length of each buffer
        std::size_t num_buffers { 0 }; // number of allocated buffers
        int res_width { 0 }; // actual capture width
        int res_height { 0 }; // actual capture height
        AVCodecContextPtr mjpeg_ctx { }; // libavcodec MJPEG decoder context
        AVPacketPtr mjpeg_packet { }; // packet wrapping the mmap'd MJPEG bytes
        AVFramePtr mjpeg_frame { }; // decoded I420 frame from MJPEG decoder

    };

}

#endif // VIDEO_CAPTURE_HH
