// related headers
#include "UdpReceiver.hh"

// c sys headers
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <endian.h>
#include <cstdint>

// cpp stdlib headers

// 3rd party headers

// project headers

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

        if (received_bytes < static_cast<ssize_t>(sizeof(PacketHeader))) return false;

        // Convert every multi-byte header field network → host. 1-byte fields
        // (version / flags / payload_type / reserved) are untouched. Mirror
        // image of UdpSender::send_raw's swap path.
        packet.header.reserved2    = ntohl(packet.header.reserved2);
        packet.header.room_id      = be64toh(packet.header.room_id);
        packet.header.peer_id      = ntohl(packet.header.peer_id);
        packet.header.ssrc         = ntohl(packet.header.ssrc);
        packet.header.timestamp    = ntohl(packet.header.timestamp);
        packet.header.sequence     = ntohs(packet.header.sequence);
        packet.header.payload_size = ntohs(packet.header.payload_size);

        // Reject any wire version we don't speak so a future v2 packet doesn't
        // get misinterpreted as a corrupted v1.
        if (packet.header.version != packet_protocol_version) return false;

        return received_bytes == static_cast<ssize_t>(packet.wire_size());

    }

}
