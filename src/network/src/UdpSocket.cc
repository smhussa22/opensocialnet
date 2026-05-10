// related headers

// c sys headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

// cpp stdlib headers

// 3rd party headers

// project headers
#include "UdpSocket.hh"

namespace OpenSocialNet::Network
{

    bool UdpSocket::open() noexcept
    {

        socket_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        return socket_fd != -1;

    }

    void UdpSocket::close() noexcept
    {

        if (!is_open()) return;
        ::close(socket_fd);
        socket_fd = -1;

    }

    bool UdpSocket::set_non_blocking() noexcept
    {

        int flags = ::fcntl(socket_fd, F_GETFL, 0);
        if (flags == -1) return false;
        return ::fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != -1;

    }

    bool UdpSocket::set_receive_buffer(int bytes) noexcept
    {

        return ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) != -1;

    }

    bool UdpSocket::set_send_buffer(int bytes) noexcept
    {

        return ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) != -1;

    }

};