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

        // Generates a random SSRC for the sender's session.
        UdpSender();
        ~UdpSender() { shutdown(); }

        UdpSender(const UdpSender&)            = delete;
        UdpSender& operator=(const UdpSender&) = delete;
        UdpSender(UdpSender&&)                 = delete;
        UdpSender& operator=(UdpSender&&)      = delete;

        // Opens the socket and resolves the remote address.
        // Returns true on success, false if the socket failed to open or the address is invalid.
        bool init(std::string_view host = ipv4_loopback_address, uint16_t port = test_port) noexcept;

        // Closes the socket and resets state.
        void shutdown() noexcept;

        // Routing identity carried in every packet from this sender. Set once
        // at session start (room_id when the user joins a voice channel,
        // peer_id when they authenticate). Both default to zero, which is the
        // "loopback / no relay" sentinel — fine for the single-machine demo.
        void set_room_id(std::uint64_t room_id) noexcept { m_room_id = room_id; }
        void set_peer_id(std::uint32_t peer_id) noexcept { m_peer_id = peer_id; }

        // Stamps the full header (version / flags / payload_type stay as the
        // caller set them) with room_id, peer_id, ssrc, seq, ts, then sends.
        // Returns true if all bytes were sent.
        bool send(Packet packet) noexcept;

        // Stamps room_id / peer_id / ssrc / seq / ts and advances the internal
        // counters WITHOUT touching the socket. Pair with send_raw() when the
        // actual TX is deferred (e.g. through LossSim) so the wire-level
        // sequence + timestamp reflect capture-order rather than the
        // (jittered) send-order. Not thread-safe; producer-thread only.
        void stamp(Packet& packet) noexcept;

        // Sends the packet exactly as given (header is caller-stamped, this
        // method only flips to network byte order before sendto). Use after
        // stamp() when the TX moment is decoupled from the stamp moment.
        bool send_raw(Packet packet) noexcept;

        bool is_open() const noexcept { return socket.is_open(); }


    private:

        UdpSocket     socket           {};   // owns the AF_INET / SOCK_DGRAM fd
        ::sockaddr_in receiver_address {};   // resolved destination set by init()
        std::uint64_t m_room_id        {};   // relay routing key; 0 means "no relay"
        std::uint32_t m_peer_id        {};   // source attribution; 0 means "anonymous"
        std::uint32_t ssrc             {};   // per-stream id, randomised at construction
        std::uint16_t sequence         {};   // increments every send; wraps at uint16 max
        std::uint32_t timestamp        {};   // media clock; increments by samples per frame

    };

}

#endif // UDP_SENDER_HH
