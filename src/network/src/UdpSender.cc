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
        ssrc = std::uniform_int_distribution<u_int32_t>{}(generator);

    }

    bool UdpSender::init(std::string_view host = ipv4_loopback_address, uint16_t port = test_port) noexcept
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

    bool UdpSender::send(Packet& packet) noexcept
    {

        packet.header.ssrc = ssrc;
        packet.header.sequence = sequence++;
        packet.header.timestamp = timestamp;
        timestamp += opus_samples_per_frame;

        ssize_t sent_bytes = ::sendto(socket.get_socket_fd(), &packet, packet.wire_size(), 0, reinterpret_cast<const sockaddr*>(&receiver_address), sizeof(receiver_address));
        return sent_bytes == static_cast<ssize_t>(packet.wire_size());

    }



};

