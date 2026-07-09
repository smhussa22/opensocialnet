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
#include "Endian.hh"       // portable <endian.h> shim
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

    bool UdpSender::init(std::string_view host, uint16_t port, uint16_t local_port) noexcept
    {

        if (!socket.open()) return false;

        // Optional local bind. The relay model requires this because the
        // server learns the client's endpoint via recvfrom's src address;
        // if we let the kernel pick an ephemeral port, recv-side replies
        // hit a different socket than the one this process is reading.
        if (local_port > 0)
        {

            sockaddr_in local { };
            local.sin_family      = AF_INET;
            local.sin_port        = htons(local_port);
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            if (::bind(socket.get_socket_fd(), reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0)
            {

                socket.close();
                return false;

            }

        }

        receiver_address.sin_family = AF_INET;
        receiver_address.sin_port   = htons(port);
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

    // Apply session-level identity + per-stream counters in host order. The
    // wire-format byte-swap is deferred to send_raw — keeping it in one
    // place means the byte order story stays in sync between this and the
    // receiver's symmetric unswap.
    void UdpSender::stamp(Packet& packet) noexcept
    {

        packet.header.version      = packet_protocol_version;
        packet.header.room_id      = m_room_id;
        packet.header.peer_id      = m_peer_id;
        packet.header.ssrc         = ssrc;
        packet.header.sequence     = sequence++;
        packet.header.timestamp    = timestamp;
        timestamp                 += opus_samples_per_frame;

    }

    bool UdpSender::send(Packet packet) noexcept
    {

        stamp(packet);
        return send_raw(std::move(packet));

    }

    bool UdpSender::send_raw(Packet packet) noexcept
    {

        const std::span<const std::uint8_t> wire { packet_to_wire(packet) };

        const ssize_t sent
        {

            ::sendto(
                socket.get_socket_fd(),
                wire.data(),
                wire.size(),
                0,
                reinterpret_cast<const ::sockaddr*>(&receiver_address),
                sizeof(receiver_address)
            )

        };

        return sent > 0;

    }

}
