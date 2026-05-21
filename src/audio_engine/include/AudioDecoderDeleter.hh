#ifndef AUDIO_DECODER_DELETER_HH
#define AUDIO_DECODER_DELETER_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <opus/opus.h>

// project headers

namespace OpenSocialNet::Audio
{

    struct AudioDecoderDeleter
    {

        void operator()(::OpusDecoder* opus_decoder) const noexcept
        {

            if (opus_decoder != nullptr) opus_decoder_destroy(opus_decoder);

        }

    };

    using OpusDecoderPtr = std::unique_ptr<::OpusDecoder, AudioDecoderDeleter>;

};

#endif // AUDIO_DECODER_DELETER_HH