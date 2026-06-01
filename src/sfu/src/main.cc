// related headers
#include "RoomRegistry.hh"
#include "SfuGrpcService.hh"

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

    // listen on all interfaces so peer containers (signaling_server) and
    // host-network probes can reach the gRPC service. 50051 is also
    // declared in src/sfu/Dockerfile's EXPOSE list and in compose.prod.yml.
    constexpr const char* listen_address { "0.0.0.0:50051" };

    OpenSocialNet::Sfu::RoomRegistry registry { };
    OpenSocialNet::Sfu::SfuGrpcService service { registry };

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

    // poll the shutdown flag from main thread; calling Server::Shutdown from
    // a signal handler deadlocks inside absl::Mutex (lock-ordering check).
    while (!shutdown_requested.load(std::memory_order_acquire))
    {

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    }

    ::printf("sfu: shutting down\n");
    server->Shutdown();
    return 0;

}
