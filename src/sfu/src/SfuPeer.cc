#include "SfuPeer.hh"

// c sys headers

// cpp stdlib headers
#include <utility>

// 3rd party headers

// project headers

namespace OpenSocialNet::Sfu
{

    bool SfuPeer::init() noexcept
    {

        try
        {

            // build the libdatachannel PeerConnection with a public STUN server
            ::rtc::Configuration config { };
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");

            peer_connection = std::make_shared<::rtc::PeerConnection>(config);

            // mirror connection state into the atomic flag; fire the one-shot peer_ready callback
            // on the first transition to Connected so subscribers (gRPC StreamPeerEvents) can
            // notify the browser via Envelope.peer_ready
            auto ready_fired { std::make_shared<std::atomic<bool>>(false) };
            peer_connection->onStateChange([this, ready_fired](::rtc::PeerConnection::State state)
            {

                if (state == ::rtc::PeerConnection::State::Connected)
                {

                    connected.store(true);
                    if (peer_ready_handler && !ready_fired->exchange(true)) peer_ready_handler();

                }
                else if (state == ::rtc::PeerConnection::State::Disconnected) connected.store(false);
                else if (state == ::rtc::PeerConnection::State::Failed) connected.store(false);
                else if (state == ::rtc::PeerConnection::State::Closed) connected.store(false);

            });

            // trickle ICE: forward every locally-gathered candidate out to subscribers as it arrives.
            // libdatachannel calls this on its own thread; the handler is responsible for thread-safety.
            peer_connection->onLocalCandidate([this](::rtc::Candidate candidate)
            {

                if (ice_candidate_handler) ice_candidate_handler(candidate.candidate(), candidate.mid());

            });

            // echo: when a remote track arrives, wire its inbound RTP back out the same track
            peer_connection->onTrack([this](std::shared_ptr<::rtc::Track> track)
            {

                // cache the track by media kind for later shutdown / fan-out
                ::rtc::Description::Media media { track->description() };
                const std::string kind { media.type() };
                if (kind == "video") video_echo_track = track;
                else if (kind == "audio") audio_echo_track = track;

                // re-emit every received RTP packet on the same bidirectional track
                std::weak_ptr<::rtc::Track> weak_track { track };
                track->onMessage([weak_track](::rtc::message_variant data)
                {

                    auto t { weak_track.lock() };
                    if (t && t->isOpen()) t->send(std::move(data));

                });

            });

            return true;

        }
        catch (...)
        {

            peer_connection.reset();
            return false;

        }

    }

    bool SfuPeer::accept_offer(std::string_view sdp_offer) noexcept
    {

        if (!peer_connection) return false;

        try
        {

            // trickle ICE: hand the offer to libdatachannel and read the answer immediately.
            // any candidates not yet gathered will trickle out via the onLocalCandidate
            // callback registered in init(), so the answer's a=candidate: lines being
            // partial here is fine and expected.
            peer_connection->setRemoteDescription(::rtc::Description { std::string { sdp_offer }, ::rtc::Description::Type::Offer });

            auto local_desc { peer_connection->localDescription() };
            if (!local_desc) return false;
            cached_answer_sdp = std::string { *local_desc };

            return true;

        }
        catch (...)
        {

            return false;

        }

    }

    bool SfuPeer::add_remote_ice_candidate(std::string_view candidate, std::string_view mid) noexcept
    {

        if (!peer_connection) return false;

        try
        {

            peer_connection->addRemoteCandidate(::rtc::Candidate { std::string { candidate }, std::string { mid } });
            return true;

        }
        catch (...)
        {

            return false;

        }

    }

    std::string SfuPeer::answer_sdp() const noexcept
    {

        return cached_answer_sdp;

    }

    bool SfuPeer::is_connected() const noexcept
    {

        return connected.load();

    }

    void SfuPeer::shutdown() noexcept
    {

        try
        {

            if (peer_connection) peer_connection->close();

        }
        catch (...)
        {

        }

        video_echo_track.reset();
        audio_echo_track.reset();
        peer_connection.reset();
        connected.store(false);

    }

    std::string_view SfuPeer::peer_id() const noexcept
    {

        return id_str;

    }

    void SfuPeer::set_peer_id(std::string id) noexcept
    {

        id_str = std::move(id);

    }

    void SfuPeer::set_local_ice_candidate_handler(IceCandidateHandler handler) noexcept
    {

        ice_candidate_handler = std::move(handler);

    }

    void SfuPeer::set_peer_ready_handler(PeerReadyHandler handler) noexcept
    {

        peer_ready_handler = std::move(handler);

    }

}
