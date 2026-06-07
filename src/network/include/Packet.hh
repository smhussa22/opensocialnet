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

    // Bump on any wire-incompatible PacketHeader change so the receiver
    // can drop unknown versions rather than silently misinterpreting bytes.
    inline constexpr std::uint8_t packet_protocol_version { 1 };


    // 1 byte payload-kind tag. Audio + video + screen-share distinguished
    // explicitly so the relay/receiver can route to the right sink without
    // a side channel. Underlying width is uint8 so the field never widens.
    enum class PayloadType : std::uint8_t
    {

        PCM         = 0, // raw PCM samples (debug / loopback)
        Opus        = 1, // opus frames (the main audio path)
        H264        = 2, // H264 camera video
        H264_Screen = 3, // H264 screen-capture video (separate so receivers tile it apart from camera)
        H265        = 4, // future codec slot

    };


    // 1-byte bitmask. Stays separate from version/payload_type so we can
    // pack extra signal cheaply (FEC, encryption, RTCP-style frames, ...)
    // without bumping the version.
    namespace PacketFlag
    {

        inline constexpr std::uint8_t marker   { 1 << 0 }; // RTP-style stream-boundary flag
        inline constexpr std::uint8_t fragment { 1 << 1 }; // this packet is one of multiple for a single frame (video)

    }


    // Wire layout, 32 bytes total. Explicit `reserved` slots keep the
    // packed-vs-natural alignment behaviour the same on every compiler
    // we'll realistically build on (gcc/clang x86_64); on those targets
    // sizeof(PacketHeader) == 32 with no internal padding the compiler
    // can sneak in.
    //
    // Field order matters: room_id sits on an 8-byte boundary because
    // it's a uint64 and we want the natural alignment, and it's also
    // the relay's primary routing key — keeping it early is mildly
    // cache-friendly when the relay only needs the first 16 bytes to
    // decide where to forward.
    struct PacketHeader
    {

        std::uint8_t  version      { packet_protocol_version }; // bumps with wire-incompatible changes
        std::uint8_t  flags        { 0 };                       // PacketFlag bitmask (marker, fragment, ...)
        PayloadType   payload_type { PayloadType::Opus };       // codec discriminator
        std::uint8_t  reserved     { 0 };                       // pad to 4-byte boundary; reserved for future flags
        std::uint32_t reserved2    { 0 };                       // pad room_id onto 8-byte alignment; reserved for future use
        std::uint64_t room_id      { 0 };                       // relay's primary routing key — 64-bit hash of channel name
        std::uint32_t peer_id      { 0 };                       // source identity, 32-bit hash of user_id (so receivers can attribute)
        std::uint32_t ssrc         { 0 };                       // per-stream id within a peer (cam / mic / screen are distinct ssrcs)
        std::uint32_t timestamp    { 0 };                       // media clock — samples elapsed (opus / PCM) or 90 kHz units (video)
        std::uint16_t sequence     { 0 };                       // monotonic per (peer_id, ssrc); wraps at uint16 max
        std::uint16_t payload_size { 0 };                       // length of the payload that follows, bytes

    };

    static_assert(sizeof(PacketHeader) == 32, "PacketHeader is the wire format; layout must be exactly 32 bytes");


    struct Packet
    {

        PacketHeader header {};
        std::uint8_t payload[maximum_packet_size] {};

        // only send the valid # of bytes through network if not full 1200.
        // technically cant fit in a uint_16t, but max packet is only 1200 max bytes + 32 byte header.
        std::uint16_t wire_size() const noexcept
        {

            return static_cast<std::uint16_t>(sizeof(PacketHeader) + header.payload_size);

        }

        std::span<std::uint8_t> valid_payload() noexcept
        {

            return { payload, header.payload_size };

        }

        std::span<const std::uint8_t> valid_payload() const noexcept
        {

            return { payload, header.payload_size };

        }

    };

}

#endif // PACKET_HH
