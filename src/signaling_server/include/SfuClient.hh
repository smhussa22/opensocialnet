#ifndef SIGNALING_SERVER_SFU_CLIENT_HH
#define SIGNALING_SERVER_SFU_CLIENT_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

// 3rd party headers
#include <grpcpp/grpcpp.h>

// project headers
#include "proto/signaling.pb.h"
#include "proto/sfu_control.pb.h"
#include "proto/sfu_control.grpc.pb.h"


namespace OpenSocialNet::Signaling
{

    // Thin RAII wrapper around the gRPC channel + stub. One instance is owned by
    // the gateway for the process lifetime; gRPC handles concurrency internally
    // so the stub is safe to share across threads.
    class SfuClient
    {

    public:

        // Callback signature for events streamed off the SFU. Runs on the
        // background reader thread; implementations must defer any
        // WebSocket work onto the uWS loop themselves.
        using EventHandler = std::function<void(const ::sfu_control::PeerEvent&)>;

        explicit SfuClient(const std::string& target);
        ~SfuClient();

        SfuClient(const SfuClient&)            = delete;
        SfuClient& operator=(const SfuClient&) = delete;

        // Synchronous AddPeer: fine for now since this is one-shot per browser
        // and not on the chat hot path. Returns true on OK; on failure the
        // `status_message` out-param gets populated.
        bool add_peer(const std::string& room_id, const std::string& peer_id, const ::signaling::Sdp& offer, ::signaling::Sdp* out_answer, std::string* status_message);

        // Fire-and-forget trickle. Logs on failure but does not block on a reply.
        void add_remote_ice_candidate(const std::string& room_id, const std::string& peer_id, const ::signaling::IceCandidate& candidate);

        // Synchronous RemovePeer; only the status is needed.
        void remove_peer(const std::string& room_id, const std::string& peer_id);

        // Sync MarkScreenShare. Tells the SFU to dispatch RTP arriving on the
        // given mid through SfuPeer's screen handler instead of the camera
        // handler. Called from on_screen_share_update right before the
        // browser renegotiates its SDP offer.
        void mark_screen_share(const std::string& room_id, const std::string& peer_id, const std::string& mid, bool sharing);

        // Sync AcceptRenegotiationAnswer. Hands the browser's SDP answer back
        // to the SFU, closing the loop on an SFU-initiated renegotiation
        // (currently the screen-share consumer track grow). Called from
        // on_client_sdp_answer.
        void accept_renegotiation_answer(const std::string& room_id, const std::string& peer_id, const ::signaling::Sdp& answer);

        // Kicks off the background reader thread that consumes PeerEvents
        // from the SFU and invokes `handler` for each. Safe to call once.
        void start_event_stream(EventHandler handler);

        // Cancels the in-flight stream RPC and joins the reader thread.
        // Idempotent.
        void shutdown();

    private:

        void event_stream_loop();

        std::shared_ptr<::grpc::Channel>                       m_channel { }; // long-lived insecure channel to 127.0.0.1:50051
        std::unique_ptr<::sfu_control::SfuControl::Stub>       m_stub { }; // generated gRPC stub
        std::thread                                            m_event_thread { }; // reader thread for StreamPeerEvents
        std::atomic<bool>                                      m_running { false }; // gate so shutdown is idempotent
        EventHandler                                           m_event_handler { }; // user callback invoked per PeerEvent

    };

}

#endif // SIGNALING_SERVER_SFU_CLIENT_HH
