#include "VideoEncoder.hh"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <vector>

void generate_test_frame(std::uint8_t* yuv420p, int width, int height, int frame_num)
{

    int y_size { width * height };
    int uv_size { (width / 2) * (height / 2) };

    std::uint8_t* y_plane { yuv420p };
    std::uint8_t* u_plane { yuv420p + y_size };
    std::uint8_t* v_plane { yuv420p + y_size + uv_size };

    // moving luma pattern, static chroma
    for (int i { 0 }; i < y_size; ++i)
    {

        y_plane[i] = static_cast<std::uint8_t>((i + frame_num * 10) % 256);

    }

    std::memset(u_plane, 128, uv_size);
    std::memset(v_plane, 128, uv_size);

}

int main()
{

    OpenSocialNet::Video::VideoEncoder encoder;

    ::printf("Testing VideoEncoder with synthetic frames\n");
    ::printf("===========================================\n\n");

    int width { 640 };
    int height { 480 };
    int fps { 30 };

    ::printf("Initializing encoder (%dx%d @ %dfps)\n", width, height, fps);
    if (!encoder.init(width, height, fps))
    {

        ::printf("ERROR: Failed to init VideoEncoder\n");
        return 1;

    }
    ::printf("  ✓ Encoder ready\n\n");

    // allocate buffers and open output file
    std::size_t yuv_size { static_cast<std::size_t>(width * height) * 3 / 2 };
    std::vector<std::uint8_t> yuv_frame(yuv_size);
    std::vector<std::byte> h264_buf(yuv_size);

    ::FILE* out { ::fopen("synthetic.h264", "wb") };
    if (!out)
    {

        ::printf("ERROR: Can't open synthetic.h264 for writing\n");
        return 1;

    }

    // generate 30 synthetic frames, encode each, write to file
    ::printf("Encoding 30 synthetic frames...\n");
    for (int i { 0 }; i < 30; ++i)
    {

        generate_test_frame(yuv_frame.data(), width, height, i);

        int h264_bytes { encoder.encode_frame(yuv_frame.data(), width, {h264_buf.data(), h264_buf.size()}) };
        if (h264_bytes <= 0)
        {

            ::printf("  [%d] encode failed\n", i);
            continue;

        }

        ::fwrite(h264_buf.data(), 1, h264_bytes, out);
        ::printf("  [%d] ✓ %d bytes H.264\n", i, h264_bytes);

    }

    // flush any remaining buffered frames
    ::printf("\nFlushing encoder...\n");
    int final_bytes { encoder.flush({h264_buf.data(), h264_buf.size()}) };
    if (final_bytes > 0)
    {

        ::fwrite(h264_buf.data(), 1, final_bytes, out);
        ::printf("  ✓ Flush: %d bytes\n", final_bytes);

    }

    ::fclose(out);

    ::printf("\n✓ Success! Wrote synthetic.h264\n");
    ::printf("Verify with: ffplay synthetic.h264\n");

    return 0;

}
