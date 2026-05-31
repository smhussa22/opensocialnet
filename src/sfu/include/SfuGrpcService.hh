#ifndef SFU_GRPC_SERVICE_HH
#define SFU_GRPC_SERVICE_HH

// related headers
#include "RoomRegistry.hh"

// c sys headers

// cpp stdlib headers

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
    // thread-safety: gRPC dispatches each RPC on a worker thread; the
    // RoomRegistry handles its own locking. service methods themselves stay
    // stateless.
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
        // browser's SDP offer, returns the SFU's answer SDP. stub: returns OK
        // with an empty answer.
        ::grpc::Status AddPeer(::grpc::ServerContext* ctx, const ::sfu_control::AddPeerRequest* req, ::sfu_control::AddPeerResponse* resp) override;

        // tears down the peer; if the room is empty afterwards, destroys it.
        // stub: returns OK with no side effects.
        ::grpc::Status RemovePeer(::grpc::ServerContext* ctx, const ::sfu_control::RemovePeerRequest* req, ::sfu_control::RemovePeerResponse* resp) override;

        // trickles a browser-side ICE candidate into the named peer.
        // stub: returns OK with no side effects.
        ::grpc::Status AddRemoteIceCandidate(::grpc::ServerContext* ctx, const ::sfu_control::AddRemoteIceCandidateRequest* req, ::sfu_control::AddRemoteIceCandidateResponse* resp) override;

        // server-streaming RPC; the SFU pushes locally-gathered ICE candidates
        // and PeerReady events for the calling signaling_server to forward to
        // browsers. stub: returns OK immediately without writing.
        ::grpc::Status StreamPeerEvents(::grpc::ServerContext* ctx, const ::sfu_control::StreamPeerEventsRequest* req, ::grpc::ServerWriter<::sfu_control::PeerEvent>* writer) override;

    private:
        RoomRegistry& registry; // injected registry; lifetime owned by main()

    };

}

#endif // SFU_GRPC_SERVICE_HH
