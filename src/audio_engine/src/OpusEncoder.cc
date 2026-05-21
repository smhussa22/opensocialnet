// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <opus/opus.h>

// project headers

namespace OpenSocialNet::Audio
{

    bool OpusEncoder::encoder_create() const noexcept
    {

        encoder = opus_encoder_create(opus_sample_rate, opus_channels, OPUS_APPLICATION_VOIP)

    }

};
