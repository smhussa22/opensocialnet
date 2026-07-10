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
    // Video frames are chopped into chunks of this many payload bytes. Header +
    // payload must stay under the ~1500 byte path MTU: full 1920-byte chunks
    // produced ~1960-byte datagrams, and the resulting IP fragments were
    // silently dropped on the EC2 path — every keyframe lost a chunk, so the
    // receiver never assembled one (dec=0 forever). 1200 matches the WebRTC
    // convention and clears every sane tunnel/VPN MTU.
    inline constexpr std::uint16_t maximum_video_chunk_size { 1200 };
    inline constexpr std::string_view ipv4_loopback_address { "127.0.0.1" };
    inline constexpr std::string_view ipv6_loopback_address { "::1" };
    inline constexpr std::uint16_t test_port { 9000 };
    inline constexpr std::uint16_t opus_samples_per_frame { 480 };
    
}


#endif // NETWORK_CONSTANTS_HH