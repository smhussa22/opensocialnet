#include "SfuGrpcService.hh"

// c sys headers

// cpp stdlib headers
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    SfuGrpcService::SfuGrpcService(RoomRegistry& registry_ref) noexcept : registry { registry_ref }
    {



    }

    ::grpc::Status SfuGrpcService::AddPeer(::grpc::ServerContext*, const ::sfu_control::AddPeerRequest* req, ::sfu_control::AddPeerResponse* resp)
    {

        const std::string& peer_id { req->peer_id() };
        const std::string& room_id { req->room_id() };
        const std::string& offer_sdp { req->offer().sdp() };

        // construct the peer; gRPC dispatches each RPC on its own worker thread
        std::unique_ptr<SfuPeer> new_peer { std::make_unique<SfuPeer>() };
        new_peer->set_peer_id(peer_id);

        // wire trickle-ICE callbacks BEFORE init() so libdatachannel's onLocalCandidate
        // and onStateChange routes here. handlers run on libdatachannel threads;
        // push_event takes the events_mutex briefly and is safe from any thread.
        std::string captured_peer_id { peer_id };
        new_peer->set_local_ice_candidate_handler([this, captured_peer_id](std::string_view cand, std::string_view mid)
        {

            PendingEvent ev { };
            ev.peer_id = captured_peer_id;
            ev.kind = PendingEvent::Kind::IceCandidate;
            ev.candidate.assign(cand.data(), cand.size());
            ev.mid.assign(mid.data(), mid.size());
            push_event(std::move(ev));

        });
        new_peer->set_peer_ready_handler([this, captured_peer_id]()
        {

            PendingEvent ev { };
            ev.peer_id = captured_peer_id;
            ev.kind = PendingEvent::Kind::PeerReady;
            push_event(std::move(ev));

        });

        if (!new_peer->init()) return ::grpc::Status { ::grpc::StatusCode::INTERNAL, "peer init failed" };

        // apply the browser SDP offer outside the map mutex would be nicer, but for Layer 1 simplicity do it inline
        if (!new_peer->accept_offer(offer_sdp)) return ::grpc::Status { ::grpc::StatusCode::INVALID_ARGUMENT, "accept_offer failed" };

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

    ::grpc::Status SfuGrpcService::RemovePeer(::grpc::ServerContext*, const ::sfu_control::RemovePeerRequest* req, ::sfu_control::RemovePeerResponse*)
    {

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

    ::grpc::Status SfuGrpcService::AddRemoteIceCandidate(::grpc::ServerContext*, const ::sfu_control::AddRemoteIceCandidateRequest* req, ::sfu_control::AddRemoteIceCandidateResponse*)
    {

        const std::string& peer_id { req->peer_id() };
        const std::string& candidate { req->candidate().candidate() };
        const std::string& mid { req->candidate().mid() };

        // look the peer up under the mutex, then invoke without holding the lock so concurrent RPCs on other peers do not stall
        SfuPeer* target { nullptr };
        {

            std::lock_guard<std::mutex> guard { peers_mutex };
            auto it { peers.find(peer_id) };
            if (it == peers.end()) return ::grpc::Status { ::grpc::StatusCode::NOT_FOUND, "no such peer" };
            target = it->second.get();

        }

        if (target != nullptr) target->add_remote_ice_candidate(candidate, mid);

        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::StreamPeerEvents(::grpc::ServerContext* ctx, const ::sfu_control::StreamPeerEventsRequest*, ::grpc::ServerWriter<::sfu_control::PeerEvent>* writer)
    {

        // drain pending events to the client until the RPC is cancelled or shutdown is requested.
        // gRPC ServerContext::IsCancelled is checked on a periodic wait_for timeout because the
        // condition_variable isn't notified on RPC cancellation by itself.
        while (!ctx->IsCancelled() && !stream_shutdown.load())
        {

            std::unique_lock<std::mutex> lock { events_mutex };
            events_cv.wait_for(lock, std::chrono::seconds { 1 }, [this]
            {

                return !events.empty() || stream_shutdown.load();

            });

            while (!events.empty())
            {

                PendingEvent ev { std::move(events.front()) };
                events.pop_front();
                lock.unlock();

                ::sfu_control::PeerEvent out { };
                out.set_peer_id(ev.peer_id);

                if (ev.kind == PendingEvent::Kind::IceCandidate)
                {

                    ::signaling::IceCandidate* ice { out.mutable_local_ice_candidate() };
                    ice->set_candidate(std::move(ev.candidate));
                    ice->set_mid(std::move(ev.mid));

                }
                else
                {

                    out.set_peer_ready(true);

                }

                if (!writer->Write(out)) return ::grpc::Status::OK;

                lock.lock();

            }

        }

        return ::grpc::Status::OK;

    }

    void SfuGrpcService::push_event(PendingEvent event) noexcept
    {

        {

            std::lock_guard<std::mutex> guard { events_mutex };
            events.push_back(std::move(event));

        }

        events_cv.notify_one();

    }

    void SfuGrpcService::request_stream_shutdown() noexcept
    {

        stream_shutdown.store(true);
        events_cv.notify_all();

    }

}