#include "SfuGrpcService.hh"

// c sys headers

// cpp stdlib headers
#include <memory>
#include <mutex>
#include <string>
#include <utility>

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

        const std::string& peer_id { req->peer_id() };
        const std::string& room_id { req->room_id() };
        const std::string& offer_sdp { req->offer().sdp() };

        // construct / reset the peer entry under the map mutex; gRPC dispatches each RPC on its own worker thread
        std::unique_ptr<SfuPeer> new_peer { std::make_unique<SfuPeer>() };
        new_peer->set_peer_id(peer_id);
        if (!new_peer->init()) return ::grpc::Status(::grpc::StatusCode::INTERNAL, "peer init failed");

        // apply the browser SDP offer outside the map mutex would be nicer, but for Layer 1 simplicity do it inline
        if (!new_peer->accept_offer(offer_sdp)) return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT, "accept_offer failed");

        // grab the cached answer before transferring ownership into the map
        std::string answer { new_peer->answer_sdp() };

        // swap into the map under the mutex; shut down any prior peer for idempotency
        {

            std::lock_guard<std::mutex> guard { peers_mutex };
            auto it { peers.find(peer_id) };
            if (it != peers.end())
            {

                if (it->second != nullptr) it->second->shutdown();
                it->second = std::move(new_peer);

            }
            else
            {

                peers.emplace(peer_id, std::move(new_peer));

            }

        }

        // populate the response Sdp message
        ::signaling::Sdp* answer_msg { resp->mutable_answer() };
        answer_msg->set_room_id(room_id);
        answer_msg->set_sdp(std::move(answer));
        answer_msg->set_type("answer");

        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::RemovePeer(::grpc::ServerContext* ctx, const ::sfu_control::RemovePeerRequest* req, ::sfu_control::RemovePeerResponse* resp)
    {

        (void)ctx;
        (void)resp;

        const std::string& peer_id { req->peer_id() };

        // pull the peer out of the map under the mutex, then shut it down once the lock is released
        std::unique_ptr<SfuPeer> evicted { };
        {

            std::lock_guard<std::mutex> guard { peers_mutex };
            auto it { peers.find(peer_id) };
            if (it != peers.end())
            {

                evicted = std::move(it->second);
                peers.erase(it);

            }

        }

        if (evicted != nullptr) evicted->shutdown();

        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::AddRemoteIceCandidate(::grpc::ServerContext* ctx, const ::sfu_control::AddRemoteIceCandidateRequest* req, ::sfu_control::AddRemoteIceCandidateResponse* resp)
    {

        (void)ctx;
        (void)resp;

        const std::string& peer_id { req->peer_id() };
        const std::string& candidate { req->candidate().candidate() };
        const std::string& mid { req->candidate().mid() };

        // look the peer up under the mutex, then invoke without holding the lock so concurrent RPCs on other peers do not stall
        SfuPeer* target { nullptr };
        {

            std::lock_guard<std::mutex> guard { peers_mutex };
            auto it { peers.find(peer_id) };
            if (it == peers.end()) return ::grpc::Status(::grpc::StatusCode::NOT_FOUND, "no such peer");
            target = it->second.get();

        }

        if (target != nullptr) target->add_remote_ice_candidate(candidate, mid);

        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::StreamPeerEvents(::grpc::ServerContext* ctx, const ::sfu_control::StreamPeerEventsRequest* req, ::grpc::ServerWriter<::sfu_control::PeerEvent>* writer)
    {

        // TODO(Layer 2): wire SfuPeer local-ICE / PeerReady callbacks into this stream so the gateway can forward them to the browser
        (void)ctx;
        (void)req;
        (void)writer;
        return ::grpc::Status::OK;

    }

}
