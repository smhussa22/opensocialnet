#ifndef OPENSOCIALNET_IPC_HH
#define OPENSOCIALNET_IPC_HH

// related headers

// c sys headers
#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

// cpp stdlib headers
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 3rd party headers
#include "proto/signaling.pb.h"

// project headers


namespace OpenSocialNet::Ipc
{

    // Wire format for parent <-> child envelope exchange over an AF_UNIX
    // SOCK_STREAM socketpair:
    //
    //   [ 4 bytes big-endian length N ] [ N bytes serialized Envelope ]
    //
    // We deliberately reuse the existing signaling.Envelope protobuf so
    // there's only one wire format end-to-end. Direction is implicit:
    //   - network child writes server->client envelopes to the parent
    //     (ChatMessageEvent, FriendRequestEvent, Ready, ...)
    //   - GUI parent writes client->server envelopes to the child
    //     (SendMessage, SendFriendRequest, FetchFriends, ...)
    // The receiver switches on payload_case() exactly like it would for
    // a real WS frame.
    //
    // Header-only so both the network binary and the native_client
    // binary include the same code without a shared object dance.

    inline constexpr std::uint32_t max_envelope_bytes { 64 * 1024 }; // generous — protobuf envelopes are <1KB in practice


    // Read exactly `n` bytes from fd into buf or fail. Loops over EINTR
    // and short reads, treats EOF / EAGAIN as failure. Returns true iff
    // every byte was read.
    inline bool read_full(int fd, void* buf, std::size_t n) noexcept
    {

        std::uint8_t* p { static_cast<std::uint8_t*>(buf) };
        std::size_t   remaining { n };
        while (remaining > 0)
        {

            const ssize_t got { ::read(fd, p, remaining) };
            if (got < 0)
            {

                if (errno == EINTR) continue;
                return false;

            }
            if (got == 0) return false; // EOF — peer closed
            p += got;
            remaining -= static_cast<std::size_t>(got);

        }
        return true;

    }


    // Write exactly `n` bytes from buf to fd or fail. Same shape as
    // read_full — loops on EINTR / short writes. Returns true iff every
    // byte was written.
    inline bool write_full(int fd, const void* buf, std::size_t n) noexcept
    {

        const std::uint8_t* p { static_cast<const std::uint8_t*>(buf) };
        std::size_t         remaining { n };
        while (remaining > 0)
        {

            const ssize_t put { ::write(fd, p, remaining) };
            if (put < 0)
            {

                if (errno == EINTR) continue;
                return false;

            }
            p += put;
            remaining -= static_cast<std::size_t>(put);

        }
        return true;

    }


    // Frame + send one Envelope over an open socketpair fd. Returns
    // false on serialization failure, oversized payload, or socket write
    // error. Holds an internal write mutex so multiple producers can
    // share the channel without interleaving partial frames.
    class Channel
    {

    public:

        Channel() noexcept = default;
        ~Channel() { stop(); }

        Channel(const Channel&)            = delete;
        Channel& operator=(const Channel&) = delete;
        Channel(Channel&&)                 = delete;
        Channel& operator=(Channel&&)      = delete;


        // Take ownership of an already-open fd. Caller (typically main())
        // has done the socketpair / fork dance. We close it in stop().
        void attach(int fd) noexcept
        {

            m_fd = fd;

        }


        // True iff a usable fd has been attached. Doesn't probe the
        // socket — a peer-closed channel still returns true until the
        // next send/recv fails and stop() is called.
        bool is_open() const noexcept { return m_fd >= 0; }


        bool send_envelope(const ::signaling::Envelope& env)
        {

            if (m_fd < 0) return false;

            std::string buf { };
            if (!env.SerializeToString(&buf)) return false;
            if (buf.size() > max_envelope_bytes) return false;

            // Note: htonl/ntohl are function-like macros on macOS (<sys/_endian.h>),
            // so they must be called unqualified — a `::` prefix expands to invalid
            // syntax. Unqualified lookup resolves to the glibc function on Linux.
            const std::uint32_t len_n { htonl(static_cast<std::uint32_t>(buf.size())) };

            std::scoped_lock<std::mutex> lock { m_write_mu };
            if (!write_full(m_fd, &len_n, sizeof(len_n))) return false;
            if (!write_full(m_fd, buf.data(), buf.size())) return false;
            return true;

        }


        // Spawn a reader thread that loops on read_envelope, dispatches
        // each parsed Envelope through `handler`. The thread terminates
        // when the peer closes the socket or stop() is invoked.
        void start_reader(std::function<void(const ::signaling::Envelope&)> handler)
        {

            if (m_reader.joinable()) return;
            m_handler = std::move(handler);
            m_running.store(true, std::memory_order_release);
            m_reader = std::thread { [this] { run_reader(); } };

        }


        // Idempotent. Closes the fd (unblocking the reader's read()),
        // joins the reader thread, releases the handler.
        void stop() noexcept
        {

            m_running.store(false, std::memory_order_release);
            if (m_fd >= 0)
            {

                ::shutdown(m_fd, SHUT_RDWR);
                ::close(m_fd);
                m_fd = -1;

            }
            if (m_reader.joinable()) m_reader.join();
            m_handler = nullptr;

        }


    private:

        void run_reader()
        {

            while (m_running.load(std::memory_order_acquire))
            {

                std::uint32_t len_n { 0 };
                if (!read_full(m_fd, &len_n, sizeof(len_n))) break;

                const std::uint32_t len { ntohl(len_n) };
                if (len == 0 or len > max_envelope_bytes) break;

                std::vector<std::uint8_t> payload(len);
                if (!read_full(m_fd, payload.data(), payload.size())) break;

                ::signaling::Envelope env { };
                if (!env.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) continue;
                if (m_handler) m_handler(env);

            }

            m_running.store(false, std::memory_order_release);

        }


        int                                                m_fd       { -1 };  // borrowed/owned socket fd; closed in stop()
        std::mutex                                         m_write_mu { };     // serialises framed writes across threads
        std::atomic<bool>                                  m_running  { false }; // reader exit flag
        std::thread                                        m_reader   { };     // joined in stop()
        std::function<void(const ::signaling::Envelope&)>  m_handler  { };     // invoked once per inbound envelope

    };

}

#endif // OPENSOCIALNET_IPC_HH
