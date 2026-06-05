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
// TODO(C++23 std::print): GCC 13 on Ubuntu 24.04 doesn't ship <print> yet
// (lands in GCC 14 / libstdc++ 14). Once the build's toolchain bumps to
// gcc-14, replace the printf calls below with std::println per
// CLAUDE.md rule 16.

// 3rd party headers
#include <rtc/rtc.hpp>

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    SfuGrpcService::SfuGrpcService(RoomRegistry& registry_ref, SfuStats& stats_ref) noexcept : registry { registry_ref }, m_stats { stats_ref }
    {



    }

    ::grpc::Status SfuGrpcService::AddPeer(::grpc::ServerContext*, const ::sfu_control::AddPeerRequest* req, ::sfu_control::AddPeerResponse* resp)
    {

        const std::string& peer_id { req->peer_id() };
        const std::string& room_id { req->room_id() };
        const std::string& offer_sdp { req->offer().sdp() };

        std::printf("sfu: AddPeer room=%s peer=%s\n", room_id.c_str(), peer_id.c_str());

        // Renegotiation fast path: a peer with this peer_id already exists,
        // so this is a browser-initiated reneg (e.g. addTrack for screen share
        // → onnegotiationneeded → fresh sdp_offer with the new m-line). Apply
        // the new offer to the EXISTING SfuPeer instead of evicting it; that
        // way the rtc::PeerConnection's ICE/DTLS state, SRTP keys, and room
        // membership all stay live and only the SDP gets renegotiated.
        std::shared_ptr<SfuPeer> existing { };
        {

            std::scoped_lock guard { peers_mutex };
            auto it { peers.find(peer_id) };
            if (it != peers.end()) existing = it->second;

        }
        if (existing != nullptr)
        {

            if (!existing->accept_offer(offer_sdp)) return ::grpc::Status { ::grpc::StatusCode::INVALID_ARGUMENT, "accept_offer (reneg) failed" };
            std::string reneg_answer { existing->answer_sdp() };
            ::signaling::Sdp* answer_msg { resp->mutable_answer() };
            answer_msg->set_room_id(room_id);
            answer_msg->set_sdp(std::move(reneg_answer));
            answer_msg->set_type("answer");
            std::printf("sfu: AddPeer renegotiated in place peer=%s\n", peer_id.c_str());
            return ::grpc::Status::OK;

        }

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
        std::string captured_room_id { room_id };
        new_peer->set_local_ice_candidate_handler([this, captured_peer_id, captured_room_id](std::string_view cand, std::string_view mid)
        {

            PendingEvent ev { };
            ev.room_id = captured_room_id;
            ev.peer_id = captured_peer_id;
            ev.kind = PendingEvent::Kind::IceCandidate;
            ev.candidate.assign(cand.data(), cand.size());
            ev.mid.assign(mid.data(), mid.size());
            push_event(std::move(ev));

        });
        new_peer->set_peer_ready_handler([this, captured_peer_id, captured_room_id]()
        {

            PendingEvent ev { };
            ev.room_id = captured_room_id;
            ev.peer_id = captured_peer_id;
            ev.kind = PendingEvent::Kind::PeerReady;
            push_event(std::move(ev));

        });
        new_peer->set_renegotiation_offer_handler([this, captured_peer_id, captured_room_id](std::string sdp)
        {

            PendingEvent ev { };
            ev.room_id = captured_room_id;
            ev.peer_id = captured_peer_id;
            ev.kind = PendingEvent::Kind::RenegotiationOffer;
            ev.sdp = std::move(sdp);
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
        new_peer->set_screen_video_rtp_handler([room, captured_peer_id](::rtc::message_variant data)
        {

            room->forward_screen_video_rtp(captured_peer_id, std::move(data));

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
        else
        {

            // brand-new peer_id, not a replacement — bump the gauge.
            m_stats.inc_active_peers();

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

        std::printf("sfu: RemovePeer peer=%s\n", peer_id.c_str());

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
        m_stats.dec_active_peers();

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
                out.set_room_id(ev.room_id);
                out.set_peer_id(ev.peer_id);

                if (ev.kind == PendingEvent::Kind::IceCandidate)
                {

                    ::signaling::IceCandidate* ice { out.mutable_local_ice_candidate() };
                    ice->set_room_id(ev.room_id);
                    ice->set_candidate(std::move(ev.candidate));
                    ice->set_mid(std::move(ev.mid));

                }
                else if (ev.kind == PendingEvent::Kind::RenegotiationOffer)
                {

                    ::signaling::Sdp* sdp { out.mutable_renegotiation_offer() };
                    sdp->set_room_id(ev.room_id);
                    sdp->set_sdp(std::move(ev.sdp));
                    sdp->set_type("offer");

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

    ::grpc::Status SfuGrpcService::MarkScreenShare(::grpc::ServerContext*, const ::sfu_control::MarkScreenShareRequest* req, ::sfu_control::MarkScreenShareResponse*)
    {

        const std::string& peer_id { req->peer_id() };
        const std::string& room_id { req->room_id() };
        const std::string& mid { req->mid() };
        const bool sharing { req->sharing() };

        std::printf("sfu: MarkScreenShare room=%s peer=%s mid=%s sharing=%d\n", room_id.c_str(), peer_id.c_str(), mid.c_str(), sharing ? 1 : 0);

        // copy the shared_ptr out under the lock so the peer stays alive past the call
        std::shared_ptr<SfuPeer> target { };
        {

            std::scoped_lock guard { peers_mutex };
            auto it { peers.find(peer_id) };
            if (it == peers.end()) return ::grpc::Status { ::grpc::StatusCode::NOT_FOUND, "no such peer" };
            target = it->second;

        }

        if (target == nullptr) return ::grpc::Status { ::grpc::StatusCode::INTERNAL, "peer entry was null" };

        if (sharing) target->mark_mid_as_screen(mid);
        else target->unmark_mid_as_screen(mid);

        // consumer-side renegotiation fanout: grow an outbound screen track on
        // every OTHER peer in the room so each viewer can receive forwarded
        // screen RTP. each peer emits a RenegotiationOffer back through its
        // event handler; the gateway ships those to browsers as
        // server_sdp_offer; browsers answer via client_sdp_answer which lands
        // here as AcceptRenegotiationAnswer. NB: late joiners that haven't
        // joined the room yet are not handled — they'd need an analogous
        // renegotiation triggered from AddPeer if any sharer is active. TODO.
        if (sharing)
        {

            Room* room { registry.find(room_id) };
            if (room != nullptr)
            {

                auto snapshot { room->snapshot_peers() };
                for (auto& peer : snapshot)
                {

                    if (!peer) continue;
                    if (peer->peer_id() == peer_id) continue;
                    peer->start_screen_consumer_renegotiation();

                }

            }

        }

        return ::grpc::Status::OK;

    }

    ::grpc::Status SfuGrpcService::AcceptRenegotiationAnswer(::grpc::ServerContext*, const ::sfu_control::AcceptRenegotiationAnswerRequest* req, ::sfu_control::AcceptRenegotiationAnswerResponse*)
    {

        const std::string& peer_id { req->peer_id() };
        const std::string& answer_sdp { req->answer().sdp() };

        std::printf("sfu: AcceptRenegotiationAnswer peer=%s sdp_size=%zu\n", peer_id.c_str(), answer_sdp.size());

        std::shared_ptr<SfuPeer> target { };
        {

            std::scoped_lock guard { peers_mutex };
            auto it { peers.find(peer_id) };
            if (it == peers.end()) return ::grpc::Status { ::grpc::StatusCode::NOT_FOUND, "no such peer" };
            target = it->second;

        }

        if (target == nullptr) return ::grpc::Status { ::grpc::StatusCode::INTERNAL, "peer entry was null" };

        if (!target->accept_renegotiation_answer(answer_sdp)) return ::grpc::Status { ::grpc::StatusCode::INVALID_ARGUMENT, "accept_renegotiation_answer failed" };

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