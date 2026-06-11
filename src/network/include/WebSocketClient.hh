#ifndef WEBSOCKET_CLIENT_HH
#define WEBSOCKET_CLIENT_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <span>
#include <string>
#include <vector>

// 3rd party headers

// project headers


namespace OpenSocialNet::Network
{

    // Tiny, blocking, single-thread RFC 6455 WebSocket client. Speaks ws://
    // only (no TLS), only the binary opcode, and assumes the peer never
    // sends a fragmented frame larger than 16 KiB — which matches the
    // signaling server's uWS::App maxPayloadLength setting. The whole point
    // is to be the smallest reasonable amount of code that gets a native
    // C++ client onto the signaling gateway without pulling a heavyweight
    // websocket lib.
    class WebSocketClient
    {

    public:

        WebSocketClient() = default;
        ~WebSocketClient();

        WebSocketClient(const WebSocketClient&)            = delete;
        WebSocketClient& operator=(const WebSocketClient&) = delete;
        WebSocketClient(WebSocketClient&&)                 = delete;
        WebSocketClient& operator=(WebSocketClient&&)      = delete;

        // TCP connect + HTTP/1.1 Upgrade handshake against ws://host:port/path.
        // Verifies Sec-WebSocket-Accept. Returns false on any error; the
        // object is left closed in that case.
        bool connect(const std::string& host, std::uint16_t port, const std::string& path);

        // Send one binary frame, masked per spec. Blocks until the whole
        // frame has been written. Returns false on socket error / closed.
        bool send_binary(std::span<const std::uint8_t> data);

        // Block until one binary frame arrives, fills `out` with its
        // payload. Returns false on socket error / close / control frame
        // we can't handle. Ping frames are auto-replied with Pong and we
        // loop; Close frames return false.
        bool recv_binary(std::vector<std::uint8_t>& out);

        // SO_RCVTIMEO in milliseconds; 0 = block forever. Used by the
        // signaling handshake so we don't wait forever for a Ready frame
        // that's never coming.
        void set_recv_timeout_ms(int ms) noexcept;

        void close() noexcept;
        bool is_open() const noexcept { return m_fd >= 0; }      // socket is currently connected


    private:

        int                       m_fd     { -1 }; // owned fd; -1 means closed
        std::vector<std::uint8_t> m_rx_buf {};     // leftover unparsed bytes between recv_binary calls

    };

}

#endif // WEBSOCKET_CLIENT_HH
