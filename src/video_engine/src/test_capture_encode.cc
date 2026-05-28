#include "VideoCapture.hh"
#include "VideoEncoder.hh"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <vector>

int main()
{

    OpenSocialNet::Video::VideoCapture capture;
    OpenSocialNet::Video::VideoEncoder encoder;

    ::printf("Testing VideoCapture → VideoEncoder\n");
    ::printf("=====================================\n\n");

    const char* device { "/dev/video0" };
    int width { 640 };
    int height { 480 };
    int fps { 30 };

    ::printf("Opening camera: %s (%dx%d @ %dfps)\n", device, width, height, fps);
    if (!capture.init(device, width, height, fps))
    {

        ::printf("ERROR: Failed to init VideoCapture\n");
        return 1;

    }
    ::printf("  ✓ Camera opened. Actual: %dx%d\n\n", capture.width(), capture.height());

    ::printf("Initializing encoder\n");
    if (!encoder.init(width, height, fps))
    {

        ::printf("ERROR: Failed to init VideoEncoder\n");
        return 1;

    }
    ::printf("  ✓ Encoder ready\n\n");

    // allocate frame buffers and open output file
    std::size_t yuv_size { static_cast<std::size_t>(width * height) * 3 / 2 };
    std::vector<std::uint8_t> yuv_frame(yuv_size);
    std::vector<std::byte> h264_buf(yuv_size);

    ::FILE* out { ::fopen("output.h264", "wb") };
    if (!out)
    {

        ::printf("ERROR: Can't open output.h264 for writing\n");
        return 1;

    }

    // capture and encode 30 frames, write H.264 bitstream to file
    ::printf("Capturing 30 frames...\n");
    int success_count { 0 };
    for (std::size_t i { 0 }; i < 30; ++i)
    {

        std::size_t bytes_captured { capture.capture_frame({yuv_frame.data(), yuv_size}) };
        if (bytes_captured == 0)
        {

            ::printf("  [%zu] capture failed\n", i);
            continue;

        }

        int h264_bytes { encoder.encode_frame(yuv_frame.data(), width, {h264_buf.data(), h264_buf.size()}) };
        if (h264_bytes <= 0)
        {

            ::printf("  [%zu] encode failed\n", i);
            continue;

        }

        ::fwrite(h264_buf.data(), 1, h264_bytes, out);
        ::printf("  [%zu] ✓ %d bytes H.264\n", i, h264_bytes);
        ++success_count;

    }

    // flush remaining encoded frames
    ::printf("\nFlushing encoder...\n");
    int final_bytes { encoder.flush({h264_buf.data(), h264_buf.size()}) };
    if (final_bytes > 0)
    {

        ::fwrite(h264_buf.data(), 1, final_bytes, out);
        ::printf("  ✓ Flush: %d bytes\n", final_bytes);

    }

    ::fclose(out);

    ::printf("\n✓ Success! Wrote %d frames to output.h264\n", success_count);
    ::printf("Verify with: ffplay output.h264\n");

    return 0;

}
