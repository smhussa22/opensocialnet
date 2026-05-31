#include "SfuGrpcService.hh"

// c sys headers

// cpp stdlib headers

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    SfuGrpcService::SfuGrpcService(RoomRegistry& registry_ref) noexcept
        : registry { registry_ref }
    {



    }

    ::grpc::Status SfuGrpcService::AddPeer(::grpc::ServerContext* ctx, const ::sfu_control::AddPeerRequest* req, ::sfu_control::AddPeerResponse* resp)
    {

        (void)ctx;
        (void)req;
        (void)resp;
        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::RemovePeer(::grpc::ServerContext* ctx, const ::sfu_control::RemovePeerRequest* req, ::sfu_control::RemovePeerResponse* resp)
    {

        (void)ctx;
        (void)req;
        (void)resp;
        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::AddRemoteIceCandidate(::grpc::ServerContext* ctx, const ::sfu_control::AddRemoteIceCandidateRequest* req, ::sfu_control::AddRemoteIceCandidateResponse* resp)
    {

        (void)ctx;
        (void)req;
        (void)resp;
        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::StreamPeerEvents(::grpc::ServerContext* ctx, const ::sfu_control::StreamPeerEventsRequest* req, ::grpc::ServerWriter<::sfu_control::PeerEvent>* writer)
    {

        (void)ctx;
        (void)req;
        (void)writer;
        return ::grpc::Status::OK;

    }

}
