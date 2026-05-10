#ifndef NETWORK_CONSTANTS_HH
#define NETWORK_CONSTANTS_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <string>

// 3rd party headers

// project headers

namespace OpenSocialNet::Network
{

    inline constexpr uint8_t maximum_packet_size { 1200 }; // via webrtc; to account for VPN, PPPoE, tunnels, etc.
    inline constexpr std::string ipv4_loopback_address { "127.0.0.1" };
    inline constexpr std::string ipv6_loopback_address { "::1" };
    
}


#endif // NETWORK_CONSTANTS_HH