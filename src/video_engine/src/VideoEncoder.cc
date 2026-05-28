#include "VideoEncoder.hh"

// c sys headers
#include <cstring>

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

        // store dimensions for later use
        width_ = width;
        height_ = height;

        // create x264 encoder parameters
        x264_param_t param;
        x264_param_default_preset(&param, "veryfast", nullptr);

        // configure encoder settings
        param.i_width = width;
        param.i_height = height;
        param.i_fps_num = framerate;
        param.i_fps_den = 1;
        param.rc.i_bitrate = 500;  // ~500kbps bitrate
        param.i_threads = 1;       // single thread for low latency
        param.b_repeat_headers = 1; // repeat SPS/PPS for each keyframe

        // apply profile constraint
        x264_param_apply_profile(&param, "main");

        // create the encoder
        x264_t* enc = x264_encoder_open(&param);
        if (!enc) {
            return false;
        }

        encoder.reset(enc);

        // initialize input picture structure
        if (x264_picture_alloc(&pic_in, X264_CSP_I420, width, height) < 0) {
            encoder.reset();
            return false;
        }

        return true;

    }

    int VideoEncoder::encode_frame(const uint8_t* yuv420p_data, int stride, std::span<std::byte> output_h264_bytes) noexcept
    {

        if (!valid()) return -1;
        if (!yuv420p_data) return -1;
        if (output_h264_bytes.empty()) return -1;

        // copy YUV420P data to pic_in
        const int uv_stride = stride / 2;
        const int uv_height = height_ / 2;

        // Y plane
        uint8_t* y_dst = pic_in.img.plane[0];
        const uint8_t* y_src = yuv420p_data;
        for (int i = 0; i < height_; ++i) {
            std::memcpy(y_dst, y_src, width_);
            y_dst += pic_in.img.i_stride[0];
            y_src += stride;
        }

        // U plane
        uint8_t* u_dst = pic_in.img.plane[1];
        const uint8_t* u_src = yuv420p_data + stride * height_;
        for (int i = 0; i < uv_height; ++i) {
            std::memcpy(u_dst, u_src, width_ / 2);
            u_dst += pic_in.img.i_stride[1];
            u_src += uv_stride;
        }

        // V plane
        uint8_t* v_dst = pic_in.img.plane[2];
        const uint8_t* v_src = yuv420p_data + stride * height_ + uv_stride * uv_height;
        for (int i = 0; i < uv_height; ++i) {
            std::memcpy(v_dst, v_src, width_ / 2);
            v_dst += pic_in.img.i_stride[2];
            v_src += uv_stride;
        }

        // encode the frame
        x264_nal_t* nals = nullptr;
        int num_nals = 0;
        int frame_bytes = x264_encoder_encode(encoder.get(), &nals, &num_nals, &pic_in);

        if (frame_bytes < 0) {
            return -1;
        }

        // write NALUs to output buffer
        int total_bytes = 0;
        for (int i = 0; i < num_nals; ++i) {
            const int nal_size = nals[i].i_payload;
            if (total_bytes + nal_size > static_cast<int>(output_h264_bytes.size())) {
                // not enough space in output buffer
                return -1;
            }

            std::memcpy(output_h264_bytes.data() + total_bytes, nals[i].p_payload, nal_size);
            total_bytes += nal_size;
        }

        return total_bytes;

    }

    int VideoEncoder::flush(std::span<std::byte> output_h264_bytes) noexcept
    {

        if (!valid()) return -1;
        if (output_h264_bytes.empty()) return -1;

        // encode with NULL picture to flush remaining frames
        x264_nal_t* nals = nullptr;
        int num_nals = 0;
        int frame_bytes = x264_encoder_encode(encoder.get(), &nals, &num_nals, nullptr);

        if (frame_bytes < 0) {
            return -1;
        }

        // write NALUs to output buffer
        int total_bytes = 0;
        for (int i = 0; i < num_nals; ++i) {
            const int nal_size = nals[i].i_payload;
            if (total_bytes + nal_size > static_cast<int>(output_h264_bytes.size())) {
                // not enough space in output buffer
                return -1;
            }

            std::memcpy(output_h264_bytes.data() + total_bytes, nals[i].p_payload, nal_size);
            total_bytes += nal_size;
        }

        return total_bytes;

    }

    void VideoEncoder::reset() noexcept
    {

        if (!valid()) return;
        x264_picture_clean(&pic_in);
        encoder.reset();

    }

    bool VideoEncoder::valid() const noexcept
    {

        return (encoder != nullptr);

    }

}
