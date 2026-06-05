#ifndef UDP_SENDER_HH
#define UDP_SENDER_HH

// related headers

// c sys headers
#include <netinet/in.h>
#include <cstdint>

// cpp stdlib headers
#include <string_view>

// 3rd party headers

// project headers
#include "UdpSocket.hh"
#include "Packet.hh"
#include "NetworkConstants.hh" 

namespace OpenSocialNet::Network
{

    class UdpSender
    {

    public:

        // Generates a random SSRC for the sender's session
        UdpSender();
        ~UdpSender() { shutdown(); }

        UdpSender(const UdpSender&) = delete;
        UdpSender& operator=(const UdpSender&) = delete;
        UdpSender(UdpSender&&) = delete;
        UdpSender& operator=(UdpSender&&) = delete;
        

        // Opens the socket and resolves the remote address.
        // Returns true on success, false if the socket failed to open or the address is invalid.
        bool init(std::string_view host = ipv4_loopback_address, uint16_t port = test_port) noexcept;

        // Closes the socket and resets state.
        void shutdown() noexcept;

        // Stamps ssrc, increments sequence, sets timestamp, then sends the packet.
        // Returns true if all bytes were sent, false on failure.
        bool send(Packet packet) noexcept;

        // Stamps ssrc / seq / ts and advances the internal counters WITHOUT
        // touching the socket. Pair with send_raw() when the actual TX is
        // deferred (e.g. through LossSim) so the wire-level sequence reflects
        // capture-order rather than the (jittered) send-order.
        // Not thread-safe; call from the producer thread only.
        void stamp(Packet& packet) noexcept;

        // Sends the packet exactly as given (header stays caller-supplied, only converts host byte order).
        // Use when the caller manages its own ssrc/sequence/timestamp clock, e.g. video.
        bool send_raw(Packet packet) noexcept;

        bool is_open() const noexcept { return socket.is_open(); }


    private:
        UdpSocket socket {};                     
        ::sockaddr_in receiver_address {};       
        std::uint32_t ssrc {};                   // unique sender ID, randomised at construction
        std::uint16_t sequence {};               // increments every packet, wraps at 65535 similar to rtp
        std::uint32_t timestamp {};              // media clock, increments by samples per frame

    };

};

#endif // UDP_SENDER_HH