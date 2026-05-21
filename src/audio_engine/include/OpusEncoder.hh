#ifndef OPUS_ENCODER_HH
#define OPUS_ENCODER_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <memory>

// 3rd party headers
#include <opus/opus.h>

// project headers
#include "AudioConstants.hh"

namespace OpenSocialNet::Audio
{

    class OpusEncoder
    {

    public:
    
        bool encoder_create() const noexcept;


    private:
        std::unique_ptr<OpusEncoder*> encoder { };


    };

};

#endif // OPUS_ENCODER_HH