#ifndef SFU_CONFIG_HH
#define SFU_CONFIG_HH

// related headers

// c sys headers
#include <cstddef>
#include <cstdint>

// cpp stdlib headers
#include <string>
#include <vector>

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    // plain-data tunables for the SFU. populated from argv/env/file at startup
    // and then read-only for the lifetime of the process. extending this struct
    // costs nothing — anything we'd otherwise hardcode lives here.
    struct SfuConfig
    {

        std::uint16_t http_signaling_port { 8080 }; // Layer 1 throwaway HTTP /offer endpoint port
        std::vector<std::string> ice_servers { "stun:stun.l.google.com:19302" }; // ICE servers handed to every PeerConnection
        std::uint16_t udp_port_min { 50000 }; // libdatachannel binds incoming media in [min, max]
        std::uint16_t udp_port_max { 60000 }; // upper bound of the media port range
        std::size_t max_peers_per_room { 32 }; // soft cap before new peers are refused
        bool log_verbose { false }; // emit per-packet trace logs (very noisy; debug only)

    };

}

#endif // SFU_CONFIG_HH
