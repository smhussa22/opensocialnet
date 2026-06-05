// related headers
#include "RoomRegistry.hh"
#include "SfuGrpcService.hh"
#include "SfuStats.hh"

// c sys headers
#include <csignal>
#include <cstdio>

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

// 3rd party headers
#include <grpcpp/grpcpp.h>
#include <rtc/rtc.hpp>

// project headers

static std::atomic<bool> shutdown_requested { false };

// signal handlers must be async-signal-safe; we only flip an atomic flag
// and let main() do the actual server teardown outside signal context.
void on_signal(int) { shutdown_requested.store(true); }

int main()
{

    // unbuffered stdout — without this, printf is line-buffered on a tty
    // but block-buffered when stdout is piped (e.g. captured by Docker's
    // log driver), so startup messages never appear in `docker logs`.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Verbose libdatachannel logger so ICE candidate gathering, DTLS setup,
    // and any silent failures during AddPeer surface in `docker logs sfu`.
    ::rtc::InitLogger(::rtc::LogLevel::Debug);

    // listen on all interfaces so peer containers (signaling_server) and
    // host-network probes can reach the gRPC service. 50051 is also
    // declared in src/sfu/Dockerfile's EXPOSE list and in compose.prod.yml.
    constexpr const char* listen_address { "0.0.0.0:50051" };

    // ownership tree: stats is at the root because RoomRegistry, Room (via
    // RoomRegistry) and SfuGrpcService all hold borrowing references to it.
    OpenSocialNet::Sfu::SfuStats stats { };
    OpenSocialNet::Sfu::RoomRegistry registry { stats };
    OpenSocialNet::Sfu::SfuGrpcService service { registry, stats };

    ::grpc::ServerBuilder builder { };
    builder.AddListeningPort(listen_address, ::grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<::grpc::Server> server { builder.BuildAndStart() };
    if (server == nullptr)
    {

        ::printf("sfu: failed to start gRPC server on %s\n", listen_address);
        return 1;

    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    ::printf("sfu: gRPC SfuControl listening on %s\n", listen_address);

    // periodic stats emitter. logs once every 10s so docker logs / EC2
    // CloudWatch tail shows a heartbeat of (rooms, peers, RTP volume) without
    // any external scraping. exits when shutdown_requested flips.
    std::thread stats_thread { [&stats]()
    {

        constexpr auto stats_period { std::chrono::seconds { 10 } };
        while (!shutdown_requested.load(std::memory_order_acquire))
        {

            std::this_thread::sleep_for(stats_period);
            if (shutdown_requested.load(std::memory_order_acquire)) break;
            stats.log_snapshot();

        }

    } };

    // poll the shutdown flag from main thread; calling Server::Shutdown from
    // a signal handler deadlocks inside absl::Mutex (lock-ordering check).
    while (!shutdown_requested.load(std::memory_order_acquire))
    {

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    }

    ::printf("sfu: shutting down\n");
    server->Shutdown();
    stats_thread.join();
    return 0;

}
