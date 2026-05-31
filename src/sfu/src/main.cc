// related headers
#include "SfuPeer.hh"

// c sys headers
#include <csignal>
#include <cstdio>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <thread>

// 3rd party headers

// project headers

static std::atomic<bool> running { true };

void on_signal(int) { running = false; }

int main()
{

    ::printf("sfu: starting (no signaling wired up yet; waiting for gRPC)\n");

    OpenSocialNet::Sfu::SfuPeer peer { };
    if (!peer.init()) ::printf("sfu: peer.init() returned false (stub)\n");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    while (running)
    {

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    }

    ::printf("sfu: shutting down\n");
    peer.shutdown();

    return 0;

}
