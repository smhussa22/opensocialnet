// Test pipeline: VideoCapture → VideoEncoder → (network) → VideoDecoder → VideoRenderer

#include "VideoCapture.hh"
#include "VideoEncoder.hh"
#include "VideoDecoder.hh"
#include "VideoRenderer.hh"
#include <cstdio>
#include <chrono>
#include <thread>

using namespace OpenSocialNet::Video;

// Example sender: capture frames, encode to H.264
void sender_pipeline(const char* device = "/dev/video0", int width = 640, int height = 480, int fps = 30) {
    VideoCapture capture;
    VideoEncoder encoder;

    printf("Sender: Initializing capture from %s (%dx%d @ %dfps)\n", device, width, height, fps);
    if (!capture.init(device, width, height, fps)) {
        printf("Failed to init capture\n");
        return;
    }

    printf("Sender: Initializing encoder\n");
    if (!encoder.init(width, height, fps)) {
        printf("Failed to init encoder\n");
        return;
    }

    // Buffer for YUV420P frame
    size_t yuv_size = width * height * 3 / 2;
    uint8_t* yuv_frame = new uint8_t[yuv_size];

    // Buffer for H.264 bitstream
    uint8_t* h264_buf = new uint8_t[yuv_size];  // conservative upper bound

    printf("Sender: Capturing and encoding 30 frames...\n");
    for (int i = 0; i < 30; ++i) {
        // Capture frame
        size_t bytes_read = capture.capture_frame({yuv_frame, yuv_size});
        if (bytes_read == 0) {
            printf("  Frame %d: capture failed\n", i);
            continue;
        }

        // Encode frame
        int h264_bytes = encoder.encode_frame(yuv_frame, width, {reinterpret_cast<std::byte*>(h264_buf), yuv_size});
        if (h264_bytes <= 0) {
            printf("  Frame %d: encode failed\n", i);
            continue;
        }

        printf("  Frame %d: %zu bytes captured, %d bytes encoded\n", i, bytes_read, h264_bytes);

        // In a real scenario, this H.264 bitstream would be sent over the network via UDP
        // For now we just print stats
    }

    // Flush encoder
    int final_bytes = encoder.flush({reinterpret_cast<std::byte*>(h264_buf), yuv_size});
    if (final_bytes > 0) {
        printf("Sender: Flush yielded %d bytes\n", final_bytes);
    }

    printf("Sender: Done\n");
    delete[] yuv_frame;
    delete[] h264_buf;
}

// Example receiver: decode H.264, render frames
void receiver_pipeline() {
    VideoDecoder decoder;
    VideoRenderer renderer;

    printf("Receiver: Initializing decoder\n");
    if (!decoder.init()) {
        printf("Failed to init decoder\n");
        return;
    }

    printf("Receiver: Initializing renderer\n");
    if (!renderer.init(640, 480, "OpenSocialNet Video Receiver")) {
        printf("Failed to init renderer\n");
        return;
    }

    // In a real scenario, H.264 packets would be received from the network
    // This is just a placeholder structure
    printf("Receiver: Ready to decode and display frames\n");
    printf("Receiver: (Would receive H.264 packets here and call decoder.decode_packet)\n");
}

int main(int argc, char** argv) {
    printf("VideoEngine Pipeline Test\n");
    printf("==========================\n\n");

    printf("SENDER PIPELINE:\n");
    printf("---------------\n");
    // Uncomment to test sender (requires camera)
    // sender_pipeline();
    printf("(skipped - requires camera device)\n\n");

    printf("RECEIVER PIPELINE:\n");
    printf("-----------------\n");
    // Uncomment to test receiver (creates window)
    // receiver_pipeline();
    printf("(skipped - requires video input)\n");

    printf("\nPipeline structure test complete.\n");
    printf("In production:\n");
    printf("  1. Sender: capture → encode → send over UDP\n");
    printf("  2. Receiver: receive UDP → decode → render\n");

    return 0;
}
