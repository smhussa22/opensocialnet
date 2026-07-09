#include "VideoEncoder.hh"

// c sys headers
#include <cstring>
#include <cstdio>

// cpp stdlib headers

// 3rd party headers
#include <x264.h>

// project headers

namespace OpenSocialNet::Video
{

    VideoEncoder::VideoEncoder() noexcept
    {

        encoder.reset();

    }

    bool VideoEncoder::init(int width, int height, int framerate) noexcept
    {

        width_ = width;
        height_ = height;

        // ultrafast + zerolatency: no B-frames, no lookahead, encoder emits a frame per input
        ::x264_param_t param { };
        ::x264_param_default_preset(&param, "ultrafast", "zerolatency");

        param.i_width = width;
        param.i_height = height;
        param.i_fps_num = framerate;
        param.i_fps_den = 1;
        param.rc.i_bitrate = 500;
        param.i_threads = 1;
        param.b_repeat_headers = 1;
        param.i_keyint_max = framerate; // keyframe every second so receivers can sync quickly
        param.b_intra_refresh = 0;
        param.i_log_level = X264_LOG_ERROR; // no banner / shutdown stats dump in user-facing output

        ::x264_param_apply_profile(&param, "baseline");

        ::x264_t* enc { ::x264_encoder_open(&param) };
        if (!enc) return false;

        encoder.reset(enc);

        if (::x264_picture_alloc(&pic_in, X264_CSP_I420, width, height) < 0)
        {

            encoder.reset();
            return false;

        }

        return true;

    }

    int VideoEncoder::encode_frame(const std::uint8_t* yuv420p_data, int stride, std::span<std::byte> output_h264_bytes) noexcept
    {

        if (!valid()) return -1;
        if (!yuv420p_data) return -1;
        if (output_h264_bytes.empty()) return -1;

        if (stride <= 0) stride = width_;

        // copy Y, U, V planes into encoder input picture
        const std::uint8_t* src { yuv420p_data };
        const int uv_h { height_ / 2 };
        const int uv_stride { stride / 2 };

        for (std::size_t i { 0 }; i < static_cast<std::size_t>(height_); ++i)
        {

            std::memcpy(pic_in.img.plane[0] + i * pic_in.img.i_stride[0], src, width_);
            src += stride;

        }

        for (std::size_t i { 0 }; i < static_cast<std::size_t>(uv_h); ++i)
        {

            std::memcpy(pic_in.img.plane[1] + i * pic_in.img.i_stride[1], src, width_ / 2);
            src += uv_stride;

        }

        for (std::size_t i { 0 }; i < static_cast<std::size_t>(uv_h); ++i)
        {

            std::memcpy(pic_in.img.plane[2] + i * pic_in.img.i_stride[2], src, width_ / 2);
            src += uv_stride;

        }

        static int frame_num { 0 };
        pic_in.i_pts = frame_num++;
        pic_in.i_type = X264_TYPE_AUTO;

        // encode and copy NAL units into output buffer; pic_out is required even if unused
        ::x264_nal_t* nals { nullptr };
        int num_nals { 0 };
        ::x264_picture_t pic_out { };
        int frame_bytes { ::x264_encoder_encode(encoder.get(), &nals, &num_nals, &pic_in, &pic_out) };

        if (frame_bytes < 0) return -1;

        int total_bytes { 0 };
        for (std::size_t i { 0 }; i < static_cast<std::size_t>(num_nals); ++i)
        {

            const int nal_size { nals[i].i_payload };
            if (total_bytes + nal_size > static_cast<int>(output_h264_bytes.size())) return -1;

            std::memcpy(output_h264_bytes.data() + total_bytes, nals[i].p_payload, nal_size);
            total_bytes += nal_size;

        }

        return total_bytes;

    }

    int VideoEncoder::flush(std::span<std::byte> output_h264_bytes) noexcept
    {

        if (!valid()) return -1;
        if (output_h264_bytes.empty()) return -1;

        // pass null picture to drain buffered frames; pic_out is required even if unused
        ::x264_nal_t* nals { nullptr };
        int num_nals { 0 };
        ::x264_picture_t pic_out { };
        int frame_bytes { ::x264_encoder_encode(encoder.get(), &nals, &num_nals, nullptr, &pic_out) };

        if (frame_bytes < 0) return -1;

        int total_bytes { 0 };
        for (std::size_t i { 0 }; i < static_cast<std::size_t>(num_nals); ++i)
        {

            const int nal_size { nals[i].i_payload };
            if (total_bytes + nal_size > static_cast<int>(output_h264_bytes.size())) return -1;

            std::memcpy(output_h264_bytes.data() + total_bytes, nals[i].p_payload, nal_size);
            total_bytes += nal_size;

        }

        return total_bytes;

    }

    void VideoEncoder::reset() noexcept
    {

        if (!valid()) return;
        ::x264_picture_clean(&pic_in);
        encoder.reset();

    }

    bool VideoEncoder::valid() const noexcept
    {

        return (encoder != nullptr);

    }

}
