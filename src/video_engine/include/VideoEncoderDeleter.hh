#ifndef VIDEO_ENCODER_DELETER_HH
#define VIDEO_ENCODER_DELETER_HH

// c sys headers
#include <cstddef>

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <x264.h>

namespace OpenSocialNet::Video
{

    struct X264EncoderDeleter
    {

        void operator()(::x264_t* encoder) const noexcept
        {

            if (encoder) ::x264_encoder_close(encoder);

        }

    };

    using X264EncoderPtr = std::unique_ptr<::x264_t, X264EncoderDeleter>;

}

#endif // VIDEO_ENCODER_DELETER_HH
