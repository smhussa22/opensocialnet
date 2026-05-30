#include "SfuPeer.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    bool SfuPeer::init() noexcept
    {

        return false;

    }

    bool SfuPeer::accept_offer(std::string_view sdp_offer) noexcept
    {

        (void)sdp_offer;
        return false;

    }

    bool SfuPeer::add_remote_ice_candidate(std::string_view candidate, std::string_view mid) noexcept
    {

        (void)candidate;
        (void)mid;
        return false;

    }

    std::string SfuPeer::answer_sdp() const noexcept
    {

        return cached_answer_sdp;

    }

    bool SfuPeer::is_connected() const noexcept
    {

        return connected.load(std::memory_order_acquire);

    }

    void SfuPeer::shutdown() noexcept
    {



    }

    std::string_view SfuPeer::peer_id() const noexcept
    {

        return id_str;

    }

    void SfuPeer::set_peer_id(std::string id) noexcept
    {

        id_str = std::move(id);

    }

}
