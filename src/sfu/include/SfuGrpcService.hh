#ifndef SFU_GRPC_SERVICE_HH
#define SFU_GRPC_SERVICE_HH

// related headers
#include "RoomRegistry.hh"
#include "SfuPeer.hh"

// c sys headers

// cpp stdlib headers
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// 3rd party headers
#include <grpcpp/grpcpp.h>

// project headers
#include "sfu_control.grpc.pb.h"

namespace OpenSocialNet::Sfu
{

    // implements the SfuControl gRPC service defined in proto/sfu_control.proto.
    // signaling_server holds the gRPC client and calls these RPCs whenever a
    // browser-side envelope (Sdp / IceCandidate / JoinVoice / LeaveVoice) lands
    // on its WebSocket gateway. each method translates a control-plane request
    // into RoomRegistry / SfuPeer operations and synthesizes the response.
    //
    // thread-safety: gRPC dispatches each RPC on a worker thread; the peer
    // tracking map below is protected by peers_mutex. each SfuPeer is also
    // co-owned by its Room (via shared_ptr) so peer lifetime survives a brief
    // window where RemovePeer races against an in-flight forwarding call.
    class SfuGrpcService final : public ::sfu_control::SfuControl::Service
    {

    public:
        // borrowing reference; the registry must outlive the service.
        explicit SfuGrpcService(RoomRegistry& registry) noexcept;
        ~SfuGrpcService() override = default;

        SfuGrpcService(const SfuGrpcService&) = delete;
        SfuGrpcService& operator=(const SfuGrpcService&) = delete;
        SfuGrpcService(SfuGrpcService&&) = delete;
        SfuGrpcService& operator=(SfuGrpcService&&) = delete;

        // builds the rtc::PeerConnection for (room_id, peer_id), applies the
        // browser's SDP offer, returns the SFU's answer SDP. idempotent on
        // peer_id; re-calling replaces any prior peer registered under the key.
        ::grpc::Status AddPeer(::grpc::ServerContext* ctx, const ::sfu_control::AddPeerRequest* req, ::sfu_control::AddPeerResponse* resp) override;

        // tears down the peer; returns OK whether or not the peer existed.
        ::grpc::Status RemovePeer(::grpc::ServerContext* ctx, const ::sfu_control::RemovePeerRequest* req, ::sfu_control::RemovePeerResponse* resp) override;

        // trickles a browser-side ICE candidate into the named peer.
        ::grpc::Status AddRemoteIceCandidate(::grpc::ServerContext* ctx, const ::sfu_control::AddRemoteIceCandidateRequest* req, ::sfu_control::AddRemoteIceCandidateResponse* resp) override;

        // server-streaming RPC; the SFU pushes locally-gathered ICE candidates
        // and PeerReady events for the calling signaling_server to forward to
        // browsers. Layer 1 stub: returns OK immediately without writing.
        ::grpc::Status StreamPeerEvents(::grpc::ServerContext* ctx, const ::sfu_control::StreamPeerEventsRequest* req, ::grpc::ServerWriter<::sfu_control::PeerEvent>* writer) override;

        // signals the StreamPeerEvents loop to exit promptly on process shutdown.
        // safe to call before the destructor; idempotent.
        void request_stream_shutdown() noexcept;

    private:
        // one event the SFU side wants to deliver to the signaling_server gateway,
        // tagged with the peer it concerns so the gateway can demux to the right WS.
        struct PendingEvent
        {

            enum class Kind { IceCandidate, PeerReady };

            std::string peer_id { }; // owning peer's id
            Kind kind { }; // discriminator: ICE candidate vs peer-ready
            std::string candidate { }; // populated when kind == IceCandidate
            std::string mid { }; // populated when kind == IceCandidate

        };

        // appends an event to the queue and wakes any waiting StreamPeerEvents loop.
        // safe to call from libdatachannel callback threads.
        void push_event(PendingEvent event) noexcept;

        // remembers which room each peer joined so RemovePeer can find the
        // Room without the caller having to repeat the room_id. value is the
        // room_id the peer was added to in AddPeer.
        RoomRegistry& registry; // injected registry; lifetime owned by main(). holds the rooms peers fan out to.
        std::unordered_map<std::string, std::shared_ptr<SfuPeer>> peers { }; // peer_id -> SfuPeer; co-owned with Room
        std::unordered_map<std::string, std::string> peer_room { }; // peer_id -> room_id, populated on AddPeer
        mutable std::mutex peers_mutex { }; // guards both maps across gRPC worker threads

        // trickle-ICE event plumbing — single shared queue drained by StreamPeerEvents.
        // signaling_server runs one long-lived subscription per process; that subscription
        // is the consumer that fans events back out to browsers over their WebSockets.
        std::mutex events_mutex { }; // guards the events deque
        std::condition_variable events_cv { }; // wakes StreamPeerEvents when an event arrives or shutdown is requested
        std::deque<PendingEvent> events { }; // pending events queued by SfuPeer callbacks
        std::atomic<bool> stream_shutdown { false }; // tells StreamPeerEvents loop to exit

    };

}

#endif // SFU_GRPC_SERVICE_HH
