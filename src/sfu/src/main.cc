// related headers
#include "SfuPeer.hh"
#include "HttpSignalingEndpoint.hh"

// c sys headers
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <thread>

// 3rd party headers

// project headers

static std::atomic<bool> running { true };

void on_signal(int) { running = false; }

int main(int argc, char** argv)
{

    std::uint16_t port { argc > 1 ? static_cast<std::uint16_t>(std::atoi(argv[1])) : static_cast<std::uint16_t>(8080) };

    ::printf("sfu: starting on http://0.0.0.0:%u (POST /offer)\n", static_cast<unsigned>(port));

    OpenSocialNet::Sfu::SfuPeer peer { };
    OpenSocialNet::Sfu::HttpSignalingEndpoint endpoint { };

    if (!peer.init()) { ::printf("peer init failed\n"); return 1; }

    auto on_offer = [&peer](std::string_view sdp_offer) -> std::string
    {

        if (!peer.accept_offer(sdp_offer)) return { };
        return peer.answer_sdp();

    };

    if (!endpoint.start(port, on_offer)) { ::printf("http endpoint start failed\n"); return 1; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    while (running)
    {

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    }

    ::printf("sfu: shutting down\n");
    endpoint.stop();
    peer.shutdown();

    return 0;

}
