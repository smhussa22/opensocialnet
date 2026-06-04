// related headers
#include "SfuClient.hh"

// c sys headers

// cpp stdlib headers
#include <iostream>
#include <string>
#include <utility>

// 3rd party headers
#include <grpcpp/grpcpp.h>

// project headers


namespace OpenSocialNet::Signaling
{

    SfuClient::SfuClient(const std::string& target)
    {

        // Build the channel + stub up front so callers can immediately issue
        // RPCs. gRPC connects lazily on the first call.
        m_channel = ::grpc::CreateChannel(target, ::grpc::InsecureChannelCredentials());
        m_stub = ::sfu_control::SfuControl::NewStub(m_channel);

    }

    SfuClient::~SfuClient()
    {

        shutdown();

    }

    bool SfuClient::add_peer(const std::string& room_id, const std::string& peer_id, const ::signaling::Sdp& offer, ::signaling::Sdp* out_answer, std::string* status_message)
    {

        ::sfu_control::AddPeerRequest request { };
        request.set_room_id(room_id);
        request.set_peer_id(peer_id);
        *request.mutable_offer() = offer;

        ::sfu_control::AddPeerResponse response { };
        ::grpc::ClientContext context { };
        const ::grpc::Status status { m_stub->AddPeer(&context, request, &response) };

        if (!status.ok())
        {

            if (status_message != nullptr) *status_message = status.error_message();
            return false;

        }

        if (out_answer != nullptr) *out_answer = response.answer();
        return true;

    }

    void SfuClient::add_remote_ice_candidate(const std::string& room_id, const std::string& peer_id, const ::signaling::IceCandidate& candidate)
    {

        ::sfu_control::AddRemoteIceCandidateRequest request { };
        request.set_room_id(room_id);
        request.set_peer_id(peer_id);
        *request.mutable_candidate() = candidate;

        ::sfu_control::AddRemoteIceCandidateResponse response { };
        ::grpc::ClientContext context { };
        const ::grpc::Status status { m_stub->AddRemoteIceCandidate(&context, request, &response) };
        if (!status.ok()) std::cerr << "[sfu] AddRemoteIceCandidate failed: " << status.error_message() << '\n';

    }

    void SfuClient::remove_peer(const std::string& room_id, const std::string& peer_id)
    {

        ::sfu_control::RemovePeerRequest request { };
        request.set_room_id(room_id);
        request.set_peer_id(peer_id);

        ::sfu_control::RemovePeerResponse response { };
        ::grpc::ClientContext context { };
        const ::grpc::Status status { m_stub->RemovePeer(&context, request, &response) };
        if (!status.ok()) std::cerr << "[sfu] RemovePeer failed: " << status.error_message() << '\n';

    }

    void SfuClient::mark_screen_share(const std::string& room_id, const std::string& peer_id, const std::string& mid, bool sharing)
    {

        ::sfu_control::MarkScreenShareRequest request { };
        request.set_room_id(room_id);
        request.set_peer_id(peer_id);
        request.set_mid(mid);
        request.set_sharing(sharing);

        ::sfu_control::MarkScreenShareResponse response { };
        ::grpc::ClientContext context { };
        const ::grpc::Status status { m_stub->MarkScreenShare(&context, request, &response) };
        if (!status.ok()) std::cerr << "[sfu] MarkScreenShare failed: " << status.error_message() << '\n';

    }

    void SfuClient::accept_renegotiation_answer(const std::string& room_id, const std::string& peer_id, const ::signaling::Sdp& answer)
    {

        ::sfu_control::AcceptRenegotiationAnswerRequest request { };
        request.set_room_id(room_id);
        request.set_peer_id(peer_id);
        *request.mutable_answer() = answer;

        ::sfu_control::AcceptRenegotiationAnswerResponse response { };
        ::grpc::ClientContext context { };
        const ::grpc::Status status { m_stub->AcceptRenegotiationAnswer(&context, request, &response) };
        if (!status.ok()) std::cerr << "[sfu] AcceptRenegotiationAnswer failed: " << status.error_message() << '\n';

    }

    void SfuClient::start_event_stream(EventHandler handler)
    {

        if (m_running.exchange(true)) return;
        m_event_handler = std::move(handler);
        m_event_thread = std::thread { [this]() { event_stream_loop(); } };

    }

    void SfuClient::shutdown()
    {

        if (!m_running.exchange(false)) return;
        // The reader thread is blocked inside Read(); the channel-level
        // shutdown causes the stream to break out with a non-OK status.
        if (m_event_thread.joinable()) m_event_thread.join();

    }

    void SfuClient::event_stream_loop()
    {

        // One outer loop so we transparently reconnect if the SFU restarts.
        while (m_running.load(std::memory_order_relaxed))
        {

            ::sfu_control::StreamPeerEventsRequest request { };
            ::grpc::ClientContext context { };
            auto reader { m_stub->StreamPeerEvents(&context, request) };

            ::sfu_control::PeerEvent event { };
            while (reader->Read(&event))
            {

                if (!m_running.load(std::memory_order_relaxed)) break;
                if (m_event_handler) m_event_handler(event);

            }

            const ::grpc::Status status { reader->Finish() };
            if (!m_running.load(std::memory_order_relaxed)) break;
            if (!status.ok()) std::cerr << "[sfu] StreamPeerEvents ended: " << status.error_message() << "; reconnecting\n";
            // Brief pause before retrying so we don't hammer a downed SFU.
            std::this_thread::sleep_for(std::chrono::seconds { 1 });

        }

    }

}
