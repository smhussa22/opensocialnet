#ifndef NETWORK_CONSTANTS_HH
#define NETWORK_CONSTANTS_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <string_view>

// 3rd party headers

// project headers

namespace OpenSocialNet::Network
{

    inline constexpr std::uint16_t maximum_packet_size { 1920 }; // 480 F32 samples (raw PCM, 10 ms @ 48 kHz); revisit once Opus encoding lands.
    inline constexpr std::string_view ipv4_loopback_address { "127.0.0.1" };
    inline constexpr std::string_view ipv6_loopback_address { "::1" };
    inline constexpr std::uint16_t test_port { 9000 };
    inline constexpr std::uint16_t opus_samples_per_frame { 480 };
    
}


#endif // NETWORK_CONSTANTS_HH