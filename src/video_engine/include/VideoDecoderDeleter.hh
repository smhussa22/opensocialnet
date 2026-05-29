#ifndef VIDEO_DECODER_DELETER_HH
#define VIDEO_DECODER_DELETER_HH

// cpp stdlib headers
#include <memory>

// 3rd party headers
extern "C"
{

#include <libavcodec/avcodec.h>

}

namespace OpenSocialNet::Video
{

    struct AVCodecContextDeleter
    {

        void operator()(::AVCodecContext* ctx) const noexcept
        {

            if (ctx) ::avcodec_free_context(&ctx);

        }

    };

    struct AVFrameDeleter
    {

        void operator()(::AVFrame* frame) const noexcept
        {

            if (frame) ::av_frame_free(&frame);

        }

    };

    struct AVPacketDeleter
    {

        void operator()(::AVPacket* packet) const noexcept
        {

            if (packet) ::av_packet_free(&packet);

        }

    };

    using AVCodecContextPtr = std::unique_ptr<::AVCodecContext, AVCodecContextDeleter>;
    using AVFramePtr = std::unique_ptr<::AVFrame, AVFrameDeleter>;
    using AVPacketPtr = std::unique_ptr<::AVPacket, AVPacketDeleter>;

}

#endif // VIDEO_DECODER_DELETER_HH
