#ifndef HTTP_SIGNALING_ENDPOINT_HH
#define HTTP_SIGNALING_ENDPOINT_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    // tiny HTTP listener used only for Layer 1 prototyping: a browser POSTs an
    // SDP offer to /offer, we invoke the registered handler, and reply with the
    // SDP answer. once Layer 2 lands, the signaling_server's Protobuf gateway
    // replaces this, and the endpoint becomes dead code we can delete.
    class HttpSignalingEndpoint
    {

    public:
        // callback signature: takes an SDP offer string, returns the SDP answer string.
        // an empty return value is treated as a failure (the endpoint emits HTTP 500).
        using OfferHandler = std::function<std::string(std::string_view sdp_offer)>;

        HttpSignalingEndpoint() noexcept = default;
        ~HttpSignalingEndpoint() { stop(); }

        HttpSignalingEndpoint(const HttpSignalingEndpoint&) = delete;
        HttpSignalingEndpoint& operator=(const HttpSignalingEndpoint&) = delete;
        HttpSignalingEndpoint(HttpSignalingEndpoint&&) = delete;
        HttpSignalingEndpoint& operator=(HttpSignalingEndpoint&&) = delete;

        // starts the HTTP server on `port`. returns true once bound + listening.
        // offer_handler runs on the listener thread; keep it short.
        bool start(std::uint16_t port, OfferHandler offer_handler) noexcept;

        // stops the listener and joins the loop thread. idempotent.
        void stop() noexcept;

        // true once start() has bound the port.
        [[nodiscard]] bool is_running() const noexcept;

    private:
        std::thread loop_thread { }; // HTTP server loop
        std::atomic<bool> running { false }; // signals loop_thread to exit
        std::uint16_t listen_port { 0 }; // port from start()
        OfferHandler handler { }; // user callback invoked per inbound offer
        std::unique_ptr<void, void(*)(void*)> impl { nullptr, [](void*){ } }; // opaque server handle (cpp-httplib server)

    };

}

#endif // HTTP_SIGNALING_ENDPOINT_HH
