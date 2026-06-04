#ifndef SFU_GRPC_SERVICE_HH
#define SFU_GRPC_SERVICE_HH

// related headers
#include "RoomRegistry.hh"
#include "SfuPeer.hh"
#include "SfuStats.hh"

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
        // borrowing references; the registry and stats must outlive the service.
        // stats is shared with RoomRegistry / Room so per-packet counter
        // updates from the forwarding path and per-connection updates from
        // gRPC worker threads land on the same aggregate.
        SfuGrpcService(RoomRegistry& registry, SfuStats& stats) noexcept;
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

        // labels a SDP mid on the named peer as carrying screen-capture video.
        // signaling_server calls this right before the browser sends its
        // renegotiated SDP offer; the SFU then routes that track's inbound
        // RTP through SfuPeer's screen handler. When sharing=true also drives
        // SFU-initiated SDP renegotiation on every OTHER peer in the room so
        // each viewer grows an outbound screen track they can receive the
        // forwarded RTP on.
        ::grpc::Status MarkScreenShare(::grpc::ServerContext* ctx, const ::sfu_control::MarkScreenShareRequest* req, ::sfu_control::MarkScreenShareResponse* resp) override;

        // closes the loop on an SFU-initiated renegotiation: the gateway
        // forwards the browser's SDP answer here, and we apply it to the
        // named peer so the new track becomes live.
        ::grpc::Status AcceptRenegotiationAnswer(::grpc::ServerContext* ctx, const ::sfu_control::AcceptRenegotiationAnswerRequest* req, ::sfu_control::AcceptRenegotiationAnswerResponse* resp) override;

        // signals the StreamPeerEvents loop to exit promptly on process shutdown.
        // safe to call before the destructor; idempotent.
        void request_stream_shutdown() noexcept;

        // process-wide counter aggregate; a future stats thread / Kafka emitter
        // grabs snapshots through this accessor without touching internals.
        [[nodiscard]] const SfuStats& stats() const noexcept { return m_stats; }
        [[nodiscard]] SfuStats& stats() noexcept { return m_stats; }

    private:
        // one event the SFU side wants to deliver to the signaling_server gateway,
        // tagged with the peer it concerns so the gateway can demux to the right WS.
        struct PendingEvent
        {

            enum class Kind { IceCandidate, PeerReady, RenegotiationOffer };

            std::string room_id { }; // owning peer's room; surfaces on the wire as PeerEvent.room_id
            std::string peer_id { }; // owning peer's id
            Kind kind { }; // discriminator: ICE candidate vs peer-ready vs renegotiation offer
            std::string candidate { }; // populated when kind == IceCandidate
            std::string mid { }; // populated when kind == IceCandidate
            std::string sdp { }; // populated when kind == RenegotiationOffer — the SFU-initiated offer SDP

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

        // borrowed counter aggregate; owned by main(). bumped here from gRPC
        // worker threads (active_peers gauge) and shared with Room for the
        // RTP packet+byte totals.
        SfuStats& m_stats;

    };

}

#endif // SFU_GRPC_SERVICE_HH
