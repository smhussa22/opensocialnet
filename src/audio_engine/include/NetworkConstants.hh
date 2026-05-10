#ifndef NETWORK_CONSTANTS_HH
#define NETWORK_CONSTANTS_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Network
{

    inline constexpr uint8_t maximum_packet_size { 1200 }; // via webrtc; to account for VPN, PPPoE, tunnels, etc.

}


#endif // NETWORK_CONSTANTS_HH