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

    constexpr const char* listen_address { "127.0.0.1:50051" };

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
