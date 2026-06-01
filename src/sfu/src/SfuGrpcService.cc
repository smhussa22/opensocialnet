#include "SfuGrpcService.hh"

// related headers
#include "Room.hh"

// c sys headers

// cpp stdlib headers
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

// 3rd party headers
#include <rtc/rtc.hpp>

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

        // construct the peer; gRPC dispatches each RPC on its own worker thread.
        // shared_ptr because Room co-owns the peer for the lifetime of its membership.
        std::shared_ptr<SfuPeer> new_peer { std::make_shared<SfuPeer>() };
        new_peer->set_peer_id(peer_id);

        // resolve / create the target room up front so the rtp handler closures can
        // capture a stable Room* by value (Room lives in registry's unordered_map
        // and we don't destroy rooms inside RemovePeer, so the pointer stays alive).
        Room* room { registry.get_or_create(room_id) };
        if (room == nullptr) return ::grpc::Status { ::grpc::StatusCode::INTERNAL, "room get_or_create returned null" };

        // wire callbacks BEFORE init() so libdatachannel's onLocalCandidate,
        // onStateChange and onTrack routes here. handlers run on libdatachannel
        // worker threads and must be short / thread-safe.
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

        // multi-room fan-out: when this peer's browser sends RTP, hand it to the
        // room which forwards to every other peer's outgoing tracks.
        new_peer->set_video_rtp_handler([room, captured_peer_id](::rtc::message_variant data)
        {

            room->forward_video_rtp(captured_peer_id, std::move(data));

        });
        new_peer->set_audio_rtp_handler([room, captured_peer_id](::rtc::message_variant data)
        {

            room->forward_audio_rtp(captured_peer_id, std::move(data));

        });

        if (!new_peer->init()) return ::grpc::Status { ::grpc::StatusCode::INTERNAL, "peer init failed" };

        // apply the browser SDP offer outside the map mutex would be nicer, but for Layer 1 simplicity do it inline
        if (!new_peer->accept_offer(offer_sdp)) return ::grpc::Status { ::grpc::StatusCode::INVALID_ARGUMENT, "accept_offer failed" };

        // grab the cached answer before handing peer ownership off to the maps + room
        std::string answer { new_peer->answer_sdp() };

        // idempotency: if a peer with this id already exists, evict it from its
        // current room and shut it down before inserting the new one.
        std::shared_ptr<SfuPeer> evicted { };
        std::string evicted_room_id { };
        {

            std::scoped_lock guard { peers_mutex };

            auto it { peers.find(peer_id) };
            if (it != peers.end())
            {

                evicted = std::move(it->second);
                auto room_it { peer_room.find(peer_id) };
                if (room_it != peer_room.end()) evicted_room_id = std::move(room_it->second);
                peers.erase(it);
                peer_room.erase(peer_id);

            }

            peers.emplace(peer_id, new_peer);
            peer_room.emplace(peer_id, room_id);

        }

        if (evicted != nullptr)
        {

            Room* old_room { evicted_room_id.empty() ? nullptr : registry.find(evicted_room_id) };
            if (old_room != nullptr) old_room->remove_peer(peer_id);
            evicted->shutdown();

        }

        room->add_peer(new_peer);

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

        // pull the peer + its room_id out under the mutex, then do the work without holding it.
        // the shared_ptr keeps the peer alive past the lock release; the room map keeps the
        // Room* valid because we don't destroy rooms in this path (Layer 1 simplicity).
        std::shared_ptr<SfuPeer> evicted { };
        std::string evicted_room_id { };
        {

            std::scoped_lock guard { peers_mutex };

            auto it { peers.find(peer_id) };
            if (it != peers.end())
            {

                evicted = std::move(it->second);
                peers.erase(it);

            }

            auto room_it { peer_room.find(peer_id) };
            if (room_it != peer_room.end())
            {

                evicted_room_id = std::move(room_it->second);
                peer_room.erase(room_it);

            }

        }

        if (evicted == nullptr) return ::grpc::Status::OK;

        Room* room { evicted_room_id.empty() ? nullptr : registry.find(evicted_room_id) };
        if (room != nullptr) room->remove_peer(peer_id);

        evicted->shutdown();

        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::AddRemoteIceCandidate(::grpc::ServerContext*, const ::sfu_control::AddRemoteIceCandidateRequest* req, ::sfu_control::AddRemoteIceCandidateResponse*)
    {

        const std::string& peer_id { req->peer_id() };
        const std::string& candidate { req->candidate().candidate() };
        const std::string& mid { req->candidate().mid() };

        // copy the shared_ptr out under the lock so the peer stays alive while we call into
        // libdatachannel without holding peers_mutex (the call may take a meaningful slice
        // of time and would otherwise stall other RPCs).
        std::shared_ptr<SfuPeer> target { };
        {

            std::scoped_lock guard { peers_mutex };
            auto it { peers.find(peer_id) };
            if (it == peers.end()) return ::grpc::Status { ::grpc::StatusCode::NOT_FOUND, "no such peer" };
            target = it->second;

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