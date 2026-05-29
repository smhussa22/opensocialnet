// related headers
#include "UdpSender.hh"

// c sys headers
#include <netinet/in.h>
#include <cstdint>

// cpp stdlib headers
#include <string>
#include <random>

// 3rd party headers

// project headers
#include "UdpSocket.hh"
#include "Packet.hh"
#include "NetworkConstants.hh" 

namespace OpenSocialNet::Network
{

    UdpSender::UdpSender()
    {

        std::mt19937 generator { std::random_device{}() };
        ssrc = std::uniform_int_distribution<std::uint32_t>{}(generator);
        
    }

    bool UdpSender::init(std::string_view host, uint16_t port) noexcept
    {

        if (!socket.open()) return false;

        receiver_address.sin_family = AF_INET;
        receiver_address.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.data(), &receiver_address.sin_addr) <= 0)
        {

            socket.close();
            return false;

        }

        return true;

    }

    void UdpSender::shutdown() noexcept
    {

        socket.close();

    }

    bool UdpSender::send(Packet packet) noexcept
    {
        packet.header.ssrc      = ssrc;
        packet.header.sequence  = sequence++;
        packet.header.timestamp = timestamp;
        timestamp              += opus_samples_per_frame;

        // convert to network byte order before sending
        uint32_t net_ssrc         = htonl(packet.header.ssrc);
        uint32_t net_timestamp    = htonl(packet.header.timestamp);
        uint16_t net_sequence     = htons(packet.header.sequence);
        uint16_t net_payload_size = htons(packet.header.payload_size);

        packet.header.ssrc         = net_ssrc;
        packet.header.timestamp    = net_timestamp;
        packet.header.sequence     = net_sequence;
        packet.header.payload_size = net_payload_size;

        ssize_t sent = ::sendto(
            socket.get_socket_fd(),
            &packet,
            ntohs(net_payload_size) + sizeof(PacketHeader),
            0,
            reinterpret_cast<const sockaddr*>(&receiver_address),
            sizeof(receiver_address)
        );

        return sent > 0;
    }

    bool UdpSender::send_raw(Packet packet) noexcept
    {

        // byte-swap caller-supplied header fields in place; payload is opaque bytes
        std::uint16_t wire_payload_size { packet.header.payload_size };

        packet.header.ssrc         = htonl(packet.header.ssrc);
        packet.header.timestamp    = htonl(packet.header.timestamp);
        packet.header.sequence     = htons(packet.header.sequence);
        packet.header.payload_size = htons(wire_payload_size);

        ssize_t sent { ::sendto(socket.get_socket_fd(), &packet, wire_payload_size + sizeof(PacketHeader), 0, reinterpret_cast<const ::sockaddr*>(&receiver_address), sizeof(receiver_address)) };

        return sent > 0;

    }

};

