// related headers
#include "RoomTable.hh"

// c sys headers
#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

// 3rd party headers

// project headers
#include "Packet.hh"

namespace osn   = OpenSocialNet::Network;
namespace relay = OpenSocialNet::Relay;

static std::atomic<bool> running { true };

static void on_signal(int) { running = false; }

static int env_int(const char* name, int fallback)
{

    const char* value { std::getenv(name) };
    if (value == nullptr or *value == '\0') return fallback;
    try { return std::stoi(value); } catch (...) { return fallback; }

}

int main()
{

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    const std::uint16_t port { static_cast<std::uint16_t>(env_int("RELAY_PORT", 50100)) };

    const int fd { ::socket(AF_INET, SOCK_DGRAM, 0) };
    if (fd < 0) { std::perror("relay: socket"); return 1; }

    sockaddr_in bind_addr { };
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = ::htons(port);
    bind_addr.sin_addr.s_addr = ::htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0)
    {

        std::perror("relay: bind");
        ::close(fd);
        return 1;

    }

    // Short recv timeout so the loop wakes ~5x/s to reap stale peers and
    // check the shutdown flag even when no traffic is flowing.
    timeval recv_timeout { 0, 200'000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    std::printf("relay: listening on udp 0.0.0.0:%u (Packet wire size %zu bytes)\n", port, sizeof(osn::PacketHeader));

    relay::RoomTable room_table { };

    // Stats. Printed in one [relay-stats] line every 10s so dockerlogs gives
    // an at-a-glance heartbeat without needing an external scrape.
    std::uint64_t packets_in    { 0 };
    std::uint64_t packets_out   { 0 };
    std::uint64_t bytes_in      { 0 };
    std::uint64_t bytes_out     { 0 };
    std::uint64_t drops_size    { 0 };  // too small to be a Packet — almost certainly garbage from a port scanner
    std::uint64_t drops_version { 0 };  // wire-format mismatch — bumped version on either end and one side wasn't redeployed

    auto last_reap  { std::chrono::steady_clock::now() };
    auto last_stats { last_reap };

    osn::Packet packet { };
    while (running.load(std::memory_order_relaxed))
    {

        sockaddr_in src     { };
        socklen_t   src_len { sizeof(src) };

        const ssize_t received_bytes
        {

            ::recvfrom(fd, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&src), &src_len)

        };

        const auto now { std::chrono::steady_clock::now() };

        if (received_bytes > 0)
        {

            ++packets_in;
            bytes_in += static_cast<std::uint64_t>(received_bytes);

            if (received_bytes < static_cast<ssize_t>(sizeof(osn::PacketHeader)))
            {

                ++drops_size;

            }
            else if (packet.header.version != osn::packet_protocol_version)
            {

                ++drops_version;

            }
            else
            {

                // Extract routing fields into host order WITHOUT mutating the
                // packet buffer — we forward the bytes verbatim to other peers,
                // so the in-memory header has to stay in network order.
                const std::uint64_t room_id { ::be64toh(packet.header.room_id) };
                const std::uint32_t peer_id { ::ntohl  (packet.header.peer_id) };

                const auto recipients { room_table.on_packet(room_id, peer_id, src, now) };

                for (const auto& addr : recipients)
                {

                    const ssize_t sent
                    {

                        ::sendto(fd, &packet, static_cast<std::size_t>(received_bytes), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr))

                    };
                    if (sent > 0)
                    {

                        ++packets_out;
                        bytes_out += static_cast<std::uint64_t>(sent);

                    }

                }

            }

        }
        // else: recv timeout (EAGAIN/EWOULDBLOCK) — fall through to the
        // periodic reap + stats checks.

        // Reap stale peers every ~5s. TTL of 30s gives the client plenty of
        // slack for transient packet loss / pause before we forget them.
        if (now - last_reap > std::chrono::seconds { 5 })
        {

            const std::size_t reaped { room_table.reap(std::chrono::seconds { 30 }, now) };
            if (reaped > 0) std::printf("relay: reaped %zu stale peer(s)\n", reaped);
            last_reap = now;

        }

        if (now - last_stats > std::chrono::seconds { 10 })
        {

            std::printf("[relay-stats] rooms=%zu peers=%zu in=%llu/%lluB out=%llu/%lluB drop_size=%llu drop_ver=%llu\n",
                room_table.room_count(), room_table.peer_count(),
                static_cast<unsigned long long>(packets_in),
                static_cast<unsigned long long>(bytes_in),
                static_cast<unsigned long long>(packets_out),
                static_cast<unsigned long long>(bytes_out),
                static_cast<unsigned long long>(drops_size),
                static_cast<unsigned long long>(drops_version));
            last_stats = now;

        }

    }

    std::printf("relay: shutting down\n");
    ::close(fd);
    return 0;

}
