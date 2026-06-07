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

    // Receive-side wrapper around a UdpSocket that someone ELSE owns. The
    // relay model forces send + recv to share one fd (so the relay's auto-
    // learned src endpoint matches where the client is actually reading),
    // so this class no longer opens its own socket — it borrows the
    // UdpSender's via UdpSender::borrow_socket() and applies the recv
    // timeout / does the byte-swap.
    //
    // Lifetime: the borrowed UdpSocket reference must outlive the UdpReceiver.
    // In practice main.cc declares the sender first, the receiver second, so
    // the destruction order is reverse — receiver destructs first.
    class UdpReceiver
    {

    public:

        UdpReceiver() = default;
        ~UdpReceiver() = default;

        UdpReceiver(const UdpReceiver&)            = delete;
        UdpReceiver& operator=(const UdpReceiver&) = delete;
        UdpReceiver(UdpReceiver&&)                 = delete;
        UdpReceiver& operator=(UdpReceiver&&)      = delete;

        // Wire up to a shared socket and set the recv timeout so receive()
        // returns ~10x/s even with no traffic, letting the caller poll a
        // running flag. Returns false if the socket isn't open yet.
        bool init(UdpSocket& shared) noexcept;

        // Drops the borrowed handle. Does NOT close the underlying socket —
        // that belongs to UdpSender.
        void shutdown() noexcept;

        // Reads one packet, byte-swaps the header fields into host order,
        // returns false on timeout / short read / unknown wire version.
        bool receive(Packet& packet) noexcept;

        bool is_open() const noexcept { return m_socket != nullptr and m_socket->is_open(); }


    private:

        UdpSocket*  m_socket       { nullptr }; // non-owning; lifetime managed by the UdpSender
        sockaddr_in sender_address { };

    };

}

#endif // UDP_RECEIVER_HH
