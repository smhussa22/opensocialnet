#include "VideoDecoder.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers
extern "C"
{

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>

}

// project headers

namespace OpenSocialNet::Video
{

    namespace
    {

        // Scan an Annex B buffer for an SPS (7) or IDR slice (5) NAL —
        // either means the stream is decodable from this packet onward
        // (the encoder repeats SPS/PPS in front of every IDR).
        bool contains_sync_nal(std::span<const std::byte> data) noexcept
        {

            const auto* p { reinterpret_cast<const std::uint8_t*>(data.data()) };
            const std::size_t n { data.size() };
            for (std::size_t i { 0 }; i + 3 < n; ++i)
            {

                if (p[i] != 0 or p[i + 1] != 0) continue;

                std::size_t nal_at { 0 };
                if (p[i + 2] == 1) nal_at = i + 3;
                else if (p[i + 2] == 0 and i + 4 < n and p[i + 3] == 1) nal_at = i + 4;
                else continue;

                const std::uint8_t nal_type { static_cast<std::uint8_t>(p[nal_at] & 0x1F) };
                if (nal_type == 7 or nal_type == 5) return true;

            }
            return false;

        }

    }

    bool VideoDecoder::init() noexcept
    {

        // find H.264 decoder, allocate context, packet, and frame
        const ::AVCodec* codec { ::avcodec_find_decoder(AV_CODEC_ID_H264) };
        if (!codec) return false;

        ::AVCodecContext* ctx { ::avcodec_alloc_context3(codec) };
        if (!ctx) return false;
        codec_ctx.reset(ctx);

        int result { ::avcodec_open2(codec_ctx.get(), codec, nullptr) };
        if (result < 0)
        {

            reset();
            return false;

        }

        ::AVPacket* pkt { ::av_packet_alloc() };
        if (!pkt)
        {

            reset();
            return false;

        }
        packet.reset(pkt);

        ::AVFrame* fr { ::av_frame_alloc() };
        if (!fr)
        {

            reset();
            return false;

        }

        frame.reset(fr);
        return true;

    }

    bool VideoDecoder::decode_packet(std::span<const std::byte> h264_data, std::uint8_t** yuv420p_planes, int* strides) noexcept
    {

        if (!valid()) return false;
        if (!yuv420p_planes || !strides) return false;

        // gate on the first keyframe: slices that reference an SPS/PPS we
        // never saw just make libavcodec scream "non-existing PPS"
        if (!synced)
        {

            if (!contains_sync_nal(h264_data)) return false;
            synced = true;

        }

        // send packet to decoder and receive decoded frame
        packet->data = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(h264_data.data()));
        packet->size = static_cast<int>(h264_data.size());

        int send_result { ::avcodec_send_packet(codec_ctx.get(), packet.get()) };
        if (send_result < 0 && send_result != AVERROR(EAGAIN) && send_result != AVERROR_EOF) return false;

        int recv_result { ::avcodec_receive_frame(codec_ctx.get(), frame.get()) };
        if (recv_result == AVERROR(EAGAIN) || recv_result == AVERROR_EOF) return false;
        if (recv_result < 0) return false;

        if (res_width == 0 || res_height == 0)
        {

            res_width = frame->width;
            res_height = frame->height;

        }

        yuv420p_planes[0] = frame->data[0];
        yuv420p_planes[1] = frame->data[1];
        yuv420p_planes[2] = frame->data[2];

        strides[0] = frame->linesize[0];
        strides[1] = frame->linesize[1];
        strides[2] = frame->linesize[2];

        return true;

    }

    bool VideoDecoder::flush(std::uint8_t** yuv420p_planes, int* strides) noexcept
    {

        if (!valid()) return false;
        if (!yuv420p_planes || !strides) return false;

        // drain remaining frames from decoder with null packet
        int send_result { ::avcodec_send_packet(codec_ctx.get(), nullptr) };
        if (send_result < 0) return false;

        int recv_result { ::avcodec_receive_frame(codec_ctx.get(), frame.get()) };
        if (recv_result == AVERROR(EAGAIN) || recv_result == AVERROR_EOF) return false;
        if (recv_result < 0) return false;

        yuv420p_planes[0] = frame->data[0];
        yuv420p_planes[1] = frame->data[1];
        yuv420p_planes[2] = frame->data[2];

        strides[0] = frame->linesize[0];
        strides[1] = frame->linesize[1];
        strides[2] = frame->linesize[2];

        return true;

    }

    void VideoDecoder::reset() noexcept
    {

        codec_ctx.reset();
        packet.reset();
        frame.reset();
        res_width = 0;
        res_height = 0;
        synced = false;

    }

    bool VideoDecoder::valid() const noexcept
    {

        return (codec_ctx != nullptr);

    }

    int VideoDecoder::width() const noexcept
    {

        return res_width;

    }

    int VideoDecoder::height() const noexcept
    {

        return res_height;

    }

}
