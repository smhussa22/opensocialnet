// related headers
#include "WebSocketClient.hh"

// c sys headers
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

// cpp stdlib headers
#include <array>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

// 3rd party headers
#include <openssl/sha.h>

// project headers


namespace OpenSocialNet::Network
{

    namespace
    {

        // 16 random bytes + base64 encode = a fresh Sec-WebSocket-Key. The
        // server hashes this with the RFC 6455 magic GUID and base64s the
        // result back as Sec-WebSocket-Accept; we use that to confirm the
        // peer actually speaks WebSocket.
        std::string base64_encode(std::span<const std::uint8_t> bytes) noexcept
        {

            static constexpr char alphabet[]
            {

                'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
                'Q','R','S','T','U','V','W','X','Y','Z','a','b','c','d','e','f',
                'g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v',
                'w','x','y','z','0','1','2','3','4','5','6','7','8','9','+','/'

            };

            std::string out { };
            out.reserve(((bytes.size() + 2) / 3) * 4);

            std::size_t i { 0 };
            while (i + 3 <= bytes.size())
            {

                const std::uint32_t triple { (static_cast<std::uint32_t>(bytes[i]) << 16) | (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | static_cast<std::uint32_t>(bytes[i + 2]) };
                out.push_back(alphabet[(triple >> 18) & 0x3f]);
                out.push_back(alphabet[(triple >> 12) & 0x3f]);
                out.push_back(alphabet[(triple >>  6) & 0x3f]);
                out.push_back(alphabet[ triple        & 0x3f]);
                i += 3;

            }

            if (i < bytes.size())
            {

                const std::uint32_t b0   { static_cast<std::uint32_t>(bytes[i]) };
                const std::uint32_t b1   { (i + 1 < bytes.size()) ? static_cast<std::uint32_t>(bytes[i + 1]) : 0u };
                const std::uint32_t pad  { static_cast<std::uint32_t>(bytes.size() - i) };
                const std::uint32_t pair { (b0 << 16) | (b1 << 8) };
                out.push_back(alphabet[(pair >> 18) & 0x3f]);
                out.push_back(alphabet[(pair >> 12) & 0x3f]);
                out.push_back(pad == 2 ? alphabet[(pair >> 6) & 0x3f] : '=');
                out.push_back('=');

            }

            return out;

        }

        std::string make_websocket_key() noexcept
        {

            std::array<std::uint8_t, 16> raw { };
            std::random_device                                rd { };
            std::uniform_int_distribution<unsigned int>       dist { 0, 255 };
            for (auto& b : raw) b = static_cast<std::uint8_t>(dist(rd));
            return base64_encode(std::span<const std::uint8_t> { raw });

        }

        std::string expected_accept(std::string_view client_key) noexcept
        {

            // RFC 6455 magic GUID. Concatenate with the client's key,
            // SHA-1, base64 → that's the value the server must echo.
            static constexpr std::string_view magic { "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" };
            std::string concat { };
            concat.reserve(client_key.size() + magic.size());
            concat.append(client_key);
            concat.append(magic);

            std::array<std::uint8_t, SHA_DIGEST_LENGTH> digest { };
            ::SHA1(reinterpret_cast<const unsigned char*>(concat.data()), concat.size(), digest.data());
            return base64_encode(std::span<const std::uint8_t> { digest });

        }

        bool write_all(int fd, const void* buf, std::size_t n) noexcept
        {

            const auto* p { static_cast<const std::uint8_t*>(buf) };
            std::size_t sent { 0 };
            while (sent < n)
            {

                const ::ssize_t k { ::send(fd, p + sent, n - sent, 0) };
                if (k <= 0) return false;
                sent += static_cast<std::size_t>(k);

            }
            return true;

        }

        // Block until exactly `n` bytes have been read into `dst`. Returns
        // false on EOF or any non-EINTR error. Used after we know a header
        // says "the next N bytes belong to this frame's payload."
        bool read_exact(int fd, std::vector<std::uint8_t>& rx_buf, std::uint8_t* dst, std::size_t n) noexcept
        {

            std::size_t copied { 0 };
            if (!rx_buf.empty())
            {

                const std::size_t take { std::min(rx_buf.size(), n) };
                std::memcpy(dst, rx_buf.data(), take);
                rx_buf.erase(rx_buf.begin(), rx_buf.begin() + static_cast<std::ptrdiff_t>(take));
                copied = take;

            }

            while (copied < n)
            {

                const ::ssize_t k { ::recv(fd, dst + copied, n - copied, 0) };
                if (k <= 0) return false;
                copied += static_cast<std::size_t>(k);

            }
            return true;

        }

        // Pull from the socket until the buffer contains "\r\n\r\n", then
        // return everything up to and including it (the HTTP header block).
        // Anything past it stays in rx_buf for the first frame parse.
        bool read_http_response_header(int fd, std::vector<std::uint8_t>& rx_buf, std::string& out_header) noexcept
        {

            out_header.clear();
            for (;;)
            {

                const auto* haystack = rx_buf.data();
                for (std::size_t i { 3 }; i < rx_buf.size(); ++i)
                {

                    if (haystack[i - 3] == '\r' and haystack[i - 2] == '\n' and haystack[i - 1] == '\r' and haystack[i] == '\n')
                    {

                        out_header.assign(reinterpret_cast<const char*>(rx_buf.data()), i + 1);
                        rx_buf.erase(rx_buf.begin(), rx_buf.begin() + static_cast<std::ptrdiff_t>(i + 1));
                        return true;

                    }

                }

                std::array<std::uint8_t, 1024> tmp { };
                const ::ssize_t k { ::recv(fd, tmp.data(), tmp.size(), 0) };
                if (k <= 0) return false;
                rx_buf.insert(rx_buf.end(), tmp.begin(), tmp.begin() + k);

            }

        }

        std::string find_http_header(std::string_view http, std::string_view name) noexcept
        {

            // very permissive HTTP header parse: case-insensitive name lookup,
            // strip surrounding whitespace from value. Good enough for the one
            // header we care about (Sec-WebSocket-Accept).
            std::string lname { name };
            for (auto& c : lname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            std::size_t pos { 0 };
            while (pos < http.size())
            {

                const std::size_t end_of_line { http.find("\r\n", pos) };
                if (end_of_line == std::string_view::npos) break;
                const std::string_view line { http.substr(pos, end_of_line - pos) };
                const std::size_t colon { line.find(':') };
                if (colon != std::string_view::npos)
                {

                    std::string hname { line.substr(0, colon) };
                    for (auto& c : hname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (hname == lname)
                    {

                        std::string_view value { line.substr(colon + 1) };
                        while (!value.empty() and (value.front() == ' ' or value.front() == '\t')) value.remove_prefix(1);
                        while (!value.empty() and (value.back()  == ' ' or value.back()  == '\t')) value.remove_suffix(1);
                        return std::string { value };

                    }

                }
                pos = end_of_line + 2;

            }
            return { };

        }

    }


    WebSocketClient::~WebSocketClient() { close(); }


    bool WebSocketClient::connect(const std::string& host, std::uint16_t port, const std::string& path)
    {

        close();

        // resolve + tcp connect
        ::addrinfo hints { };
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        char port_str[16] { };
        std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(port));

        ::addrinfo* res { nullptr };
        if (::getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 or res == nullptr)
        {

            std::fprintf(stderr, "[ws] getaddrinfo(%s:%u) failed\n", host.c_str(), port);
            return false;

        }

        int fd { -1 };
        for (auto* ai { res }; ai != nullptr; ai = ai->ai_next)
        {

            fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
            ::close(fd);
            fd = -1;

        }
        ::freeaddrinfo(res);
        if (fd < 0)
        {

            std::fprintf(stderr, "[ws] tcp connect to %s:%u failed\n", host.c_str(), port);
            return false;

        }

        // build + send HTTP upgrade
        const std::string client_key { make_websocket_key() };
        std::string req { };
        req.reserve(256);
        req.append("GET ").append(path).append(" HTTP/1.1\r\n");
        req.append("Host: ").append(host).append(":").append(port_str).append("\r\n");
        req.append("Upgrade: websocket\r\n");
        req.append("Connection: Upgrade\r\n");
        req.append("Sec-WebSocket-Version: 13\r\n");
        req.append("Sec-WebSocket-Key: ").append(client_key).append("\r\n");
        req.append("\r\n");

        if (!write_all(fd, req.data(), req.size()))
        {

            std::fprintf(stderr, "[ws] failed to write upgrade request\n");
            ::close(fd);
            return false;

        }

        // parse response
        std::vector<std::uint8_t> rx_buf { };
        std::string header { };
        if (!read_http_response_header(fd, rx_buf, header))
        {

            std::fprintf(stderr, "[ws] no upgrade response\n");
            ::close(fd);
            return false;

        }

        if (header.find("HTTP/1.1 101") != 0)
        {

            std::fprintf(stderr, "[ws] upgrade rejected: %.40s\n", header.c_str());
            ::close(fd);
            return false;

        }

        const std::string accept_value { find_http_header(header, "Sec-WebSocket-Accept") };
        const std::string expected     { expected_accept(client_key) };
        if (accept_value != expected)
        {

            std::fprintf(stderr, "[ws] Sec-WebSocket-Accept mismatch (got=%s expected=%s)\n", accept_value.c_str(), expected.c_str());
            ::close(fd);
            return false;

        }

        m_fd = fd;
        m_rx_buf = std::move(rx_buf);
        return true;

    }


    void WebSocketClient::set_recv_timeout_ms(int ms) noexcept
    {

        if (m_fd < 0) return;
        ::timeval tv { };
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    }


    bool WebSocketClient::send_binary(std::span<const std::uint8_t> data)
    {

        if (m_fd < 0) return false;

        // build header: FIN=1, opcode=0x2 (binary), MASK=1
        std::array<std::uint8_t, 14> hdr { };
        std::size_t hdr_len { 0 };
        hdr[hdr_len++] = 0x82; // 1000 0010

        const std::size_t n { data.size() };
        if (n <= 125)
        {

            hdr[hdr_len++] = static_cast<std::uint8_t>(0x80 | n);

        }
        else if (n <= 0xFFFF)
        {

            hdr[hdr_len++] = static_cast<std::uint8_t>(0x80 | 126);
            hdr[hdr_len++] = static_cast<std::uint8_t>((n >> 8) & 0xFF);
            hdr[hdr_len++] = static_cast<std::uint8_t>( n       & 0xFF);

        }
        else
        {

            hdr[hdr_len++] = static_cast<std::uint8_t>(0x80 | 127);
            for (int i { 7 }; i >= 0; --i) hdr[hdr_len++] = static_cast<std::uint8_t>((n >> (i * 8)) & 0xFF);

        }

        // 4-byte masking key
        std::array<std::uint8_t, 4> mask { };
        std::random_device                          rd { };
        std::uniform_int_distribution<unsigned int> dist { 0, 255 };
        for (auto& b : mask) b = static_cast<std::uint8_t>(dist(rd));
        std::memcpy(hdr.data() + hdr_len, mask.data(), 4);
        hdr_len += 4;

        if (!write_all(m_fd, hdr.data(), hdr_len)) return false;

        // mask + send payload in stack chunks so we never copy the whole
        // thing at once for 16 KiB payloads.
        std::array<std::uint8_t, 1024> chunk { };
        std::size_t i { 0 };
        while (i < n)
        {

            const std::size_t take { std::min<std::size_t>(chunk.size(), n - i) };
            for (std::size_t j { 0 }; j < take; ++j) chunk[j] = data[i + j] ^ mask[(i + j) & 3];
            if (!write_all(m_fd, chunk.data(), take)) return false;
            i += take;

        }

        return true;

    }


    bool WebSocketClient::recv_binary(std::vector<std::uint8_t>& out)
    {

        if (m_fd < 0) return false;

        for (;;)
        {

            // read 2-byte fixed header
            std::array<std::uint8_t, 2> head { };
            if (!read_exact(m_fd, m_rx_buf, head.data(), 2)) return false;

            const bool         fin    { (head[0] & 0x80) != 0 };
            const std::uint8_t opcode { static_cast<std::uint8_t>(head[0] & 0x0F) };
            const bool         masked { (head[1] & 0x80) != 0 };
            std::uint64_t      len    { static_cast<std::uint64_t>(head[1] & 0x7F) };

            if (len == 126)
            {

                std::array<std::uint8_t, 2> ext { };
                if (!read_exact(m_fd, m_rx_buf, ext.data(), 2)) return false;
                len = (static_cast<std::uint64_t>(ext[0]) << 8) | static_cast<std::uint64_t>(ext[1]);

            }
            else if (len == 127)
            {

                std::array<std::uint8_t, 8> ext { };
                if (!read_exact(m_fd, m_rx_buf, ext.data(), 8)) return false;
                len = 0;
                for (auto b : ext) len = (len << 8) | b;

            }

            std::array<std::uint8_t, 4> mask { };
            if (masked and !read_exact(m_fd, m_rx_buf, mask.data(), 4)) return false;

            std::vector<std::uint8_t> payload(static_cast<std::size_t>(len));
            if (len > 0 and !read_exact(m_fd, m_rx_buf, payload.data(), payload.size())) return false;
            if (masked)
            {

                for (std::size_t i { 0 }; i < payload.size(); ++i) payload[i] ^= mask[i & 3];

            }

            if (!fin)
            {

                std::fprintf(stderr, "[ws] fragmented frames not supported\n");
                return false;

            }

            switch (opcode)
            {

                case 0x2: // binary
                    out = std::move(payload);
                    return true;

                case 0x9: // ping → reply pong with the same payload
                {

                    std::array<std::uint8_t, 14> phdr { };
                    std::size_t phdr_len { 0 };
                    phdr[phdr_len++] = 0x8A; // FIN + pong
                    const std::size_t pn { payload.size() };
                    if (pn <= 125) phdr[phdr_len++] = static_cast<std::uint8_t>(0x80 | pn);
                    else if (pn <= 0xFFFF)
                    {

                        phdr[phdr_len++] = static_cast<std::uint8_t>(0x80 | 126);
                        phdr[phdr_len++] = static_cast<std::uint8_t>((pn >> 8) & 0xFF);
                        phdr[phdr_len++] = static_cast<std::uint8_t>( pn       & 0xFF);

                    }
                    else { std::fprintf(stderr, "[ws] ping too large\n"); return false; }

                    std::array<std::uint8_t, 4> pmask { };
                    std::random_device                          rd { };
                    std::uniform_int_distribution<unsigned int> dist { 0, 255 };
                    for (auto& b : pmask) b = static_cast<std::uint8_t>(dist(rd));
                    std::memcpy(phdr.data() + phdr_len, pmask.data(), 4);
                    phdr_len += 4;

                    if (!write_all(m_fd, phdr.data(), phdr_len)) return false;
                    for (std::size_t i { 0 }; i < pn; ++i) payload[i] ^= pmask[i & 3];
                    if (pn > 0 and !write_all(m_fd, payload.data(), pn)) return false;
                    continue;

                }

                case 0xA: // pong → ignore
                    continue;

                case 0x8: // close
                    std::fprintf(stderr, "[ws] peer sent Close\n");
                    return false;

                default:
                    std::fprintf(stderr, "[ws] unsupported opcode 0x%x\n", opcode);
                    return false;

            }

        }

    }


    void WebSocketClient::close() noexcept
    {

        if (m_fd >= 0)
        {

            ::close(m_fd);
            m_fd = -1;

        }
        m_rx_buf.clear();

    }

}
