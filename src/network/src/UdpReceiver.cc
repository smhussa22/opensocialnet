// related headers
#include "UdpReceiver.hh"

// c sys headers
#include <netinet/in.h>
#include <cstdint>

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Network
{

    bool UdpReceiver::init(uint16_t port) noexcept
    {

        if (!socket.open()) return false;

        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = INADDR_ANY;

        if (::bind(socket.get_socket_fd(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) == -1)
        {

            socket.close();
            return false;
            
        }

        return true;

    }

    void UdpReceiver::shutdown() noexcept
    {

        socket.close();

    }

    bool UdpReceiver::receive(Packet& packet) noexcept
    {

        socklen_t sender_address_size { sizeof(sender_address) };
        ssize_t received_bytes = ::recvfrom(socket.get_socket_fd(), &packet, packet.wire_size(), 0, reinterpret_cast<sockaddr*>(&sender_address), &sender_address_size);
        if (received_bytes < static_cast<ssize_t>(sizeof(PacketHeader))) return false;
        return received_bytes == static_cast<ssize_t>(packet.wire_size());

    }

};