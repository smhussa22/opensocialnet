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

            // build the libdatachannel PeerConnection with a public STUN server.
            // narrow the UDP port range libdatachannel binds for ICE so Docker
            // doesn't have to install 10000+ iptables NAT rules on container
            // start (the default ephemeral range made `docker compose up`
            // hang for minutes on EC2). 200 ports = up to ~100 concurrent
            // peers — plenty for a single-instance deploy.
            ::rtc::Configuration config { };
            config.iceServers.emplace_back("stun:stun.l.google.com:19302");
            config.portRangeBegin = 50000;
            config.portRangeEnd = 50200;

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

                std::printf("sfu: onLocalCandidate peer=%s mid=%s cand=%s handler_set=%d\n", id_str.c_str(), candidate.mid().c_str(), candidate.candidate().c_str(), (ice_candidate_handler ? 1 : 0));
                if (ice_candidate_handler) ice_candidate_handler(candidate.candidate(), candidate.mid());

            });

            // local description firehose. fires for BOTH the initial answer (in
            // response to the browser's first offer applied via accept_offer)
            // AND any later SFU-initiated offer (start_screen_consumer_renegotiation).
            // we only route the Offer flavour out — Answers go back to the browser
            // via accept_offer's synchronous localDescription() read, so emitting
            // them here would double-deliver.
            peer_connection->onLocalDescription([this](::rtc::Description desc)
            {

                if (desc.type() != ::rtc::Description::Type::Offer) return;
                if (renegotiation_offer_handler) renegotiation_offer_handler(std::string { desc });

            });

            // echo: when a remote track arrives, wire its inbound RTP back out the same track
            peer_connection->onTrack([this](std::shared_ptr<::rtc::Track> track)
            {

                // cache the track by media kind for later shutdown / fan-out
                ::rtc::Description::Media media { track->description() };
                const std::string kind { media.type() };
                const std::string mid { media.mid() };

                if (kind == "video") video_echo_track = track;
                else if (kind == "audio") audio_echo_track = track;

                // hand inbound rtp off to whoever registered as handler
                // handler is sfugrpcservicer w/ room, peer id and forwards into teh room.
                // for video tracks we additionally peek into screen_mids: if signaling has
                // labelled this mid as a screen capture (via mark_mid_as_screen, called
                // before the renegotiated SDP offer arrived), the inbound RTP goes through
                // the screen handler instead of the camera handler so consumers don't
                // overwrite their camera tile.
                track->onMessage([this, kind, mid](::rtc::message_variant data)
                {

                    if (kind == "video")
                    {

                        bool is_screen { false };
                        {

                            std::scoped_lock guard { screen_mids_mutex };
                            is_screen = screen_mids.find(mid) != screen_mids.end();

                        }

                        if (is_screen && screen_video_rtp_handler) screen_video_rtp_handler(std::move(data));
                        else if (video_rtp_handler) video_rtp_handler(std::move(data));

                    }
                    else if (kind == "audio" && audio_rtp_handler)
                    {

                        audio_rtp_handler(std::move(data));

                    }

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
        screen_video_track.reset();
        peer_connection.reset();
        connected.store(false);
        renegotiation_in_flight.store(false);

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

    void SfuPeer::set_audio_rtp_handler (RtpHandler handler) noexcept
    {

        audio_rtp_handler = std::move(handler);

    }

    void SfuPeer::set_video_rtp_handler (RtpHandler handler) noexcept
    {

        video_rtp_handler = std::move(handler);

    }

    void SfuPeer::send_audio_rtp (::rtc::message_variant data) noexcept
    {

        try
        {

            if (audio_echo_track && audio_echo_track->isOpen()) audio_echo_track->send(std::move(data));

        }
        catch(...)
        {


        }


    }

    void SfuPeer::send_video_rtp (::rtc::message_variant data) noexcept
    {

        try
        {

            if (video_echo_track and video_echo_track->isOpen()) video_echo_track->send(std::move(data));

        }
        catch(...)
        {


        }

    }

    // ---- screen share ----
    // Sharer side: mark_mid_as_screen + the screen-aware onTrack dispatch in
    // init() route inbound RTP on the marked mid to screen_video_rtp_handler.
    // Consumer side: start_screen_consumer_renegotiation grows an outbound
    // screen track and drives an SFU-initiated SDP offer; accept_renegotiation_answer
    // closes the cycle when the browser's answer arrives.

    void SfuPeer::set_screen_video_rtp_handler (RtpHandler handler) noexcept
    {

        screen_video_rtp_handler = std::move(handler);

    }

    void SfuPeer::send_screen_video_rtp (::rtc::message_variant data) noexcept
    {

        try
        {

            if (screen_video_track && screen_video_track->isOpen()) screen_video_track->send(std::move(data));

        }
        catch (...)
        {



        }

    }

    void SfuPeer::mark_mid_as_screen (std::string mid) noexcept
    {

        if (mid.empty()) return;
        std::scoped_lock guard { screen_mids_mutex };
        screen_mids.insert(std::move(mid));

    }

    void SfuPeer::unmark_mid_as_screen (const std::string& mid) noexcept
    {

        if (mid.empty()) return;
        std::scoped_lock guard { screen_mids_mutex };
        screen_mids.erase(mid);

    }

    bool SfuPeer::has_screen_track () const noexcept
    {

        return screen_video_track && screen_video_track->isOpen();

    }

    // ---- SFU-initiated renegotiation ----

    void SfuPeer::set_renegotiation_offer_handler (RenegotiationOfferHandler handler) noexcept
    {

        renegotiation_offer_handler = std::move(handler);

    }

    bool SfuPeer::start_screen_consumer_renegotiation () noexcept
    {

        if (!peer_connection) return false;

        // glare guard — refuse to overlap two SFU-initiated renegotiations.
        // accept_renegotiation_answer flips this back to false on success.
        bool expected { false };
        if (!renegotiation_in_flight.compare_exchange_strong(expected, true)) return false;

        try
        {

            // idempotent: if we already have a screen track from a prior cycle,
            // skip the addTrack and just re-fire setLocalDescription so the
            // browser gets a fresh offer (e.g. on reconnect / glare retry).
            if (!screen_video_track)
            {

                ::rtc::Description::Video media { "screen", ::rtc::Description::Direction::SendOnly };
                // libdatachannel needs a payload type registered on the m-line
                // for SDP generation to succeed. VP8 / 96 matches what the
                // existing camera echo tracks ride on (browsers default-offer
                // VP8 at PT 96), so the SFU's screen forward is wire-compatible
                // with whatever payload format the sharer's RTP arrives as.
                media.addVP8Codec(96);
                screen_video_track = peer_connection->addTrack(media);

            }

            // explicit setLocalDescription on top of the addTrack causes
            // libdatachannel to (re)emit the local SDP as an Offer through
            // onLocalDescription, which we route to renegotiation_offer_handler.
            peer_connection->setLocalDescription();
            return true;

        }
        catch (...)
        {

            renegotiation_in_flight.store(false);
            return false;

        }

    }

    bool SfuPeer::accept_renegotiation_answer (std::string_view sdp_answer) noexcept
    {

        if (!peer_connection) return false;

        try
        {

            peer_connection->setRemoteDescription(::rtc::Description { std::string { sdp_answer }, ::rtc::Description::Type::Answer });
            renegotiation_in_flight.store(false);
            return true;

        }
        catch (...)
        {

            // leave in_flight set so callers can see something is wrong;
            // shutdown() will clear it when the peer is torn down.
            return false;

        }

    }

}
