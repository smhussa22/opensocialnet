#include "SfuPeer.hh"

// c sys headers

// cpp stdlib headers
#include <chrono>
#include <future>
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

            // mirror the underlying connection state into the atomic connected flag
            peer_connection->onStateChange([this](::rtc::PeerConnection::State state)
            {

                if (state == ::rtc::PeerConnection::State::Connected) connected.store(true);
                else if (state == ::rtc::PeerConnection::State::Disconnected) connected.store(false);
                else if (state == ::rtc::PeerConnection::State::Failed) connected.store(false);
                else if (state == ::rtc::PeerConnection::State::Closed) connected.store(false);

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

            // wire up a per-offer gathering-complete promise BEFORE setRemoteDescription so we
            // can't race the gathering callback firing before we install the waiter
            auto gathering_done { std::make_shared<std::promise<void>>() };
            std::future<void> gathering_future { gathering_done->get_future() };
            auto already_signalled { std::make_shared<std::atomic<bool>>(false) };

            peer_connection->onGatheringStateChange([gathering_done, already_signalled](::rtc::PeerConnection::GatheringState state)
            {

                if (state != ::rtc::PeerConnection::GatheringState::Complete) return;
                if (already_signalled->exchange(true)) return;
                gathering_done->set_value();

            });

            // cover the (unlikely) case where gathering already completed before the callback was installed
            if (peer_connection->gatheringState() == ::rtc::PeerConnection::GatheringState::Complete)
            {

                if (!already_signalled->exchange(true)) gathering_done->set_value();

            }

            // hand the browser's offer to libdatachannel; it will auto-generate the matching answer
            peer_connection->setRemoteDescription(::rtc::Description(std::string(sdp_offer), ::rtc::Description::Type::Offer));

            // non-trickle ICE: block until all candidates are gathered, then read the full local SDP
            if (gathering_future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) return false;

            auto local_desc { peer_connection->localDescription() };
            if (!local_desc) return false;
            cached_answer_sdp = std::string(*local_desc);

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

            peer_connection->addRemoteCandidate(::rtc::Candidate(std::string(candidate), std::string(mid)));
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

}
