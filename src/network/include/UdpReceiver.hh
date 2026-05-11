#ifndef UDP_RECEIVER_HH
#define UDP_RECEIVER_HH

// related headers

// c sys headers
#include <netinet/in.h>
#include <cstdint>

// cpp stdlib headers

// 3rd party headers

// project headers
#include "UdpSocket.hh"
#include "Packet.hh"
#include "NetworkConstants.hh"

namespace OpenSocialNet::Network
{

    class UdpReceiver
    {

    public:
        UdpReceiver() = default;
        ~UdpReceiver() {  shutdown(); }

        UdpReceiver(const UdpReceiver&) = delete;
        UdpReceiver& operator=(const UdpReceiver&) = delete;
        UdpReceiver(UdpReceiver&&) = delete;
        UdpReceiver& operator=(UdpReceiver&&) = delete;

        // Opens the socket and binds to the given port on all interfaces.
        // Returns true on success, false if the socket failed to open or bind.
        bool init(uint16_t port = test_port) noexcept;

        // Closes the socket and resets state.
        void shutdown() noexcept;

        // Reads one packet into the provided Packet struct.
        // Returns true if a packet was received, false if nothing was available or an error occurred.
        bool receive(Packet& packet) noexcept;

        bool is_open() const noexcept { return socket.is_open(); }
    
    private:
        UdpSocket socket {};
        sockaddr_in sender_address {};
        
    };

};

#endif // UDP_RECEIVER_HH