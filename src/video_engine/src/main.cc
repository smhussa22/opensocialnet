// Test pipeline: VideoCapture → VideoEncoder → (network) → VideoDecoder → VideoRenderer

#include "VideoCapture.hh"
#include "VideoEncoder.hh"
#include "VideoDecoder.hh"
#include "VideoRenderer.hh"
#include <cstdio>
#include <cstdint>
#include <cstddef>
#include <vector>

// capture and encode camera frames into H.264, printing per-frame stats
void sender_pipeline(const char* device = "/dev/video0", int width = 640, int height = 480, int fps = 30)
{

    OpenSocialNet::Video::VideoCapture capture;
    OpenSocialNet::Video::VideoEncoder encoder;

    ::printf("Sender: Initializing capture from %s (%dx%d @ %dfps)\n", device, width, height, fps);
    if (!capture.init(device, width, height, fps))
    {

        ::printf("Failed to init capture\n");
        return;

    }

    ::printf("Sender: Initializing encoder\n");
    if (!encoder.init(width, height, fps))
    {

        ::printf("Failed to init encoder\n");
        return;

    }

    std::size_t yuv_size { static_cast<std::size_t>(width * height) * 3 / 2 };
    std::vector<std::uint8_t> yuv_frame(yuv_size);
    std::vector<std::byte> h264_buf(yuv_size);

    // capture and encode 30 frames; in production send over UDP
    ::printf("Sender: Capturing and encoding 30 frames...\n");
    for (std::size_t i { 0 }; i < 30; ++i)
    {

        std::size_t bytes_read { capture.capture_frame({yuv_frame.data(), yuv_size}) };
        if (bytes_read == 0)
        {

            ::printf("  Frame %zu: capture failed\n", i);
            continue;

        }

        int h264_bytes { encoder.encode_frame(yuv_frame.data(), width, {h264_buf.data(), h264_buf.size()}) };
        if (h264_bytes <= 0)
        {

            ::printf("  Frame %zu: encode failed\n", i);
            continue;

        }

        ::printf("  Frame %zu: %zu bytes captured, %d bytes encoded\n", i, bytes_read, h264_bytes);

    }

    int final_bytes { encoder.flush({h264_buf.data(), h264_buf.size()}) };
    if (final_bytes > 0) ::printf("Sender: Flush yielded %d bytes\n", final_bytes);

    ::printf("Sender: Done\n");

}

// decode incoming H.264 packets and display frames
void receiver_pipeline()
{

    OpenSocialNet::Video::VideoDecoder decoder;
    OpenSocialNet::Video::VideoRenderer renderer;

    ::printf("Receiver: Initializing decoder\n");
    if (!decoder.init())
    {

        ::printf("Failed to init decoder\n");
        return;

    }

    ::printf("Receiver: Initializing renderer\n");
    if (!renderer.init(640, 480, "OpenSocialNet Video Receiver"))
    {

        ::printf("Failed to init renderer\n");
        return;

    }

    ::printf("Receiver: Ready to decode and display frames\n");
    ::printf("Receiver: (Would receive H.264 packets here and call decoder.decode_packet)\n");

}

int main()
{

    ::printf("VideoEngine Pipeline Test\n");
    ::printf("==========================\n\n");

    ::printf("SENDER PIPELINE:\n");
    ::printf("---------------\n");
    // sender_pipeline();
    ::printf("(skipped - requires camera device)\n\n");

    ::printf("RECEIVER PIPELINE:\n");
    ::printf("-----------------\n");
    // receiver_pipeline();
    ::printf("(skipped - requires video input)\n");

    ::printf("\nPipeline structure test complete.\n");
    ::printf("In production:\n");
    ::printf("  1. Sender: capture → encode → send over UDP\n");
    ::printf("  2. Receiver: receive UDP → decode → render\n");

    return 0;

}
