#ifndef AUDIO_ENCODER_DELETER_HH
#define AUDIO_ENCODER_DELETER_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <opus/opus.h>

// project headers

namespace OpenSocialNet::Audio
{

    struct AudioEncoderDeleter
    {

        void operator()(::OpusEncoder* opus_encoder) const noexcept
        {

            if (opus_encoder != nullptr) opus_encoder_destroy(opus_encoder);

        }

    };

    using OpusEncoderPtr = std::unique_ptr<::OpusEncoder, AudioEncoderDeleter>;

};

#endif // AUDIO_ENCODER_DELETER_HH
