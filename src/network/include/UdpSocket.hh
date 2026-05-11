#ifndef UDP_SOCKET_HH
#define UDP_SOCKET_HH

// related headers

// c sys headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// cpp stdlib headers

// 3rd party headers

// project headers


namespace OpenSocialNet::Network
{

    class UdpSocket
    {

    public:    
        UdpSocket() = default;
        ~UdpSocket() { ::close(socket_fd); }
        
        UdpSocket(const UdpSocket&) = delete;                       // avoid copying so 2 objects dont think they own the same fd
        UdpSocket& operator=(const UdpSocket&) = delete;            // same reason as above for the copyassignment
        UdpSocket(UdpSocket&& other) noexcept : socket_fd { other.socket_fd }
        { 
            
            other.socket_fd = -1; 
        
        }
        UdpSocket& operator=(UdpSocket&&) = delete;                 // just never needed

        // Creates the UDP socket file descriptor. !!!! MUST !!!! be called before any other method
        // Returns true on success, false if the OS failed to create the socket.
        bool open() noexcept;

        // Releases the file descriptor back to the OS. Safe to call multiple times.
        void close() noexcept;

        // Makes recvfrom return immediately when no data is available instead of blocking.
        // Returns true on success, false if the OS failed to set the flag.
        bool set_non_blocking() noexcept;           

        // Sets the kernel receive buffer to the given size in bytes.
        // Returns true on success, false if the OS rejected the buffer size
        bool set_receive_buffer(int bytes) noexcept;

        // Sets the kernel send buffer to the given size in bytes.
        // Returns true on success, false if the OS rejected the buffer size.
        bool set_send_buffer(int bytes) noexcept;

        bool is_open() const noexcept { return socket_fd != -1; }
        int get_socket_fd() const noexcept { return socket_fd; }


    private:
        int socket_fd { -1 }; // -1 implies invalid/no socket_fd, at initialization

    };

};

#endif // UDP_SOCKET_HH