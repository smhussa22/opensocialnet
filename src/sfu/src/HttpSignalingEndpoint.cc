#include "HttpSignalingEndpoint.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    bool HttpSignalingEndpoint::start(std::uint16_t port, OfferHandler offer_handler) noexcept
    {

        
        return false;

    }

    void HttpSignalingEndpoint::stop() noexcept
    {



    }

    bool HttpSignalingEndpoint::is_running() const noexcept
    {

        return running.load(std::memory_order_acquire);

    }

}
