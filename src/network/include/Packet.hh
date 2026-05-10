#ifndef PACKET_HH
#define PACKET_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <span>

// 3rd party headers

// project headers
#include "NetworkConstants.hh"

namespace OpenSocialNet::Network
{

    enum class PayloadType : uint8_t
    {

        PCM  = 0,
        Opus = 1,
        H264 = 2,
        H265 = 3,

    };

    struct PacketHeader
    {

        std::uint32_t ssrc {};              // sender ID; random at stream start
        std::uint32_t timestamp {};         // media clock, samples elapsed, not wall clock
        std::uint16_t sequence {};          // increments every packet, wraps at max
        std::uint16_t payload_size {};      // bytes of payload
        PayloadType payload_type {};        // PCM or Opus
        std::uint8_t marker {};             // stream boundary flag
        std::uint16_t padding {};           // 2 byte padding; prefer over alignas for clarity

    };

    struct Packet
    {

        PacketHeader header {};
        std::uint8_t payload[maximum_packet_size] {};
        
        // to only send the valid # of bytes through network if not full 1200
        // technically cant fit in a uint_16t, but max packet is only 1200 max bytes + 16 byte header
        std::uint16_t wire_size() const noexcept 
        {

            return (sizeof(PacketHeader) + header.payload_size); 

        }

        std::span<std::uint8_t> valid_payload() noexcept
        {

            return {payload, header.payload_size};

        }

        std::span<const std::uint8_t> valid_payload() const noexcept
        {

            return {payload, header.payload_size};

        }

    };

}

#endif // PACKET_HH