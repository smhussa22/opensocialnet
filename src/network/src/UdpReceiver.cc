// related headers
#include "UdpReceiver.hh"

// c sys headers
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <cstdint>

// cpp stdlib headers

// 3rd party headers

// project headers
#include "Endian.hh"       // portable <endian.h> shim

namespace OpenSocialNet::Network
{

    bool UdpReceiver::init(UdpSocket& shared) noexcept
    {

        m_socket = &shared;
        if (!m_socket->is_open()) { m_socket = nullptr; return false; }

        // Short blocking timeout so the recv thread loop wakes ~10x/s and can
        // notice the running flag flipping to false. Same value the previous
        // owned-socket version used.
        timeval timeout { 0, 100'000 };
        ::setsockopt(m_socket->get_socket_fd(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        return true;

    }

    void UdpReceiver::shutdown() noexcept
    {

        m_socket = nullptr;

    }

    bool UdpReceiver::receive(Packet& packet) noexcept
    {

        if (m_socket == nullptr) return false;

        socklen_t     sender_address_size { sizeof(sender_address) };
        const ssize_t received_bytes
        {

            ::recvfrom(m_socket->get_socket_fd(), &packet, sizeof(Packet), 0, reinterpret_cast<sockaddr*>(&sender_address), &sender_address_size)

        };

        if (received_bytes < 0) return false;
        return packet_from_wire(packet, static_cast<std::size_t>(received_bytes));

    }

}
