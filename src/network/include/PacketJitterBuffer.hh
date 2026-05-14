#ifndef PACKET_JITTER_BUFFER_HH
#define PACKET_JITTER_BUFFER_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <map>

// 3rd party headers

// project headers
#include "Packet.hh"

namespace OpenSocialNet::Network
{

    class PacketJitterBuffer 
    {
     
    public:

        PacketJitterBuffer() = default;
        ~PacketJitterBuffer() = default;

        PacketJitterBuffer(const PacketJitterBuffer&) = delete;
        PacketJitterBuffer& operator=(const PacketJitterBuffer&) = delete;
        PacketJitterBuffer(PacketJitterBuffer&&) = delete;
        PacketJitterBuffer& operator=(PacketJitterBuffer&&) = delete;
        
        // inserts packet keyed by sequence number, discards if too late
        bool push(Packet& packet) noexcept;

        // returns the next packet in order if ready, returns false if not enough buffered yet
        bool pop(Packet& out) noexcept;

        // returns how many packets are currently held
        size_t get_size() const noexcept;

        void set_playout_threshold(size_t threshold) noexcept { playout_threshold = threshold; }
        size_t size() const noexcept { return buffer.size(); }
        bool is_playing() const noexcept { return playing; }

    private:
        std::map<std::uint16_t, Packet> buffer {};     // packets keyed by sequence number, autoordered
        std::uint16_t next_sequence {};                // sequence number expected to play next
        size_t playout_threshold { 5 };                // minimum packets buffered before playback starts
        bool playing { false };                        // true once there is enough packets to start

    };


};

#endif // JITTER_BUFFER_HH