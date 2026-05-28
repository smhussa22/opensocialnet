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

namespace OpenSocialNet::Video
{
    class VideoCapture
    {
    public:
        explicit VideoCapture() noexcept;
        ~VideoCapture() { shutdown(); }

        VideoCapture(const VideoCapture&) = delete;
        VideoCapture& operator=(const VideoCapture&) = delete;
        VideoCapture(VideoCapture&&) = delete;
        VideoCapture& operator=(VideoCapture&&) = delete;

        // opens video device and starts capturing. returns true on success.
        bool init(const char* device_path, int width, int height, int framerate) noexcept;

        // stops capture and closes video device.
        void shutdown() noexcept;

        // reads next captured frame as YUV420P data. returns frame size in bytes.
        size_t capture_frame(std::span<uint8_t> frame_buffer) noexcept;

        // returns whether capture device was successfully initialized.
        [[nodiscard]] bool valid() const noexcept;

        // returns frame width/height.
        [[nodiscard]] int width() const noexcept;
        [[nodiscard]] int height() const noexcept;

    private:
        int device_fd {-1};
        uint8_t* buffers[4] {};
        size_t buffer_lengths[4] {};
        int num_buffers {0};
        int width_ {0};
        int height_ {0};
    };

}

#endif // VIDEO_CAPTURE_HH