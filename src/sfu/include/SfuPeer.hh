#ifndef SFU_PEER_HH
#define SFU_PEER_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

// 3rd party headers
#include <rtc/rtc.hpp>

// project headers

namespace OpenSocialNet::Sfu
{

    // single browser-to-server WebRTC peer. Layer 1 behaviour: echo whatever
    // media (video + audio) the browser sends back to the same browser. Higher
    // layers (multi-peer forwarding, rooms) will compose this primitive.
    class SfuPeer
    {

    public:
        SfuPeer() noexcept = default;
        ~SfuPeer() { shutdown(); }

        SfuPeer(const SfuPeer&) = delete;
        SfuPeer& operator=(const SfuPeer&) = delete;
        SfuPeer(SfuPeer&&) = delete;
        SfuPeer& operator=(SfuPeer&&) = delete;

        // builds the libdatachannel PeerConnection and registers state / track callbacks.
        // must succeed before accept_offer can be called.
        bool init() noexcept;

        // accepts a remote SDP offer (browser side), generates the matching answer,
        // and caches it. answer is then retrievable via answer_sdp().
        bool accept_offer(std::string_view sdp_offer) noexcept;

        // remote ICE candidate from the browser. fed in once received via signaling.
        bool add_remote_ice_candidate(std::string_view candidate, std::string_view mid) noexcept;

        // SDP answer produced by accept_offer. empty until accept_offer succeeds.
        [[nodiscard]] std::string answer_sdp() const noexcept;

        // true once the underlying libdatachannel state reaches Connected.
        [[nodiscard]] bool is_connected() const noexcept;

        // closes the peer connection, releases tracks, returns to pre-init state.
        void shutdown() noexcept;

        // returns the human-readable identifier assigned by the owning Room. empty until set.
        [[nodiscard]] std::string_view peer_id() const noexcept;

        // assigns this peer's identifier; called by Room when the peer joins.
        void set_peer_id(std::string id) noexcept;

        // trickle ICE event handlers — register BEFORE calling init() so the
        // libdatachannel callbacks installed inside init() have somewhere to
        // dispatch to. handlers run on libdatachannel internal threads; they
        // must be short and thread-safe.
        using IceCandidateHandler = std::function<void(std::string_view candidate, std::string_view mid)>;
        using PeerReadyHandler = std::function<void()>;

        // invoked for each locally-gathered ICE candidate libdatachannel emits.
        void set_local_ice_candidate_handler(IceCandidateHandler handler) noexcept;

        // invoked once when the peer connection first reaches Connected.
        void set_peer_ready_handler(PeerReadyHandler handler) noexcept;

        // give a peer a way to know which room it is in
        using RtpHandler = std::function<void(::rtc::message_variant data)>;

        // Room calls these to push outbound media to this peer's browser.
        // writes go to the outgoing track libdatachannel created during the
        // SDP offer/answer exchange. no-op if the relevant track isn't open.
        void set_audio_rtp_handler (RtpHandler handler) noexcept;
        void set_video_rtp_handler (RtpHandler handler) noexcept;
        void send_audio_rtp (::rtc::message_variant data) noexcept;
        void send_video_rtp (::rtc::message_variant data) noexcept;

        // screen share. A second video m-line is added to the existing
        // PeerConnection when the browser starts sharing; libdatachannel
        // surfaces it through onTrack like any other video track. We
        // distinguish it from the camera by the SDP `mid` identifier the
        // browser tells us about via signaling::ScreenShareUpdate, which
        // EnvelopeHandlers forwards into mark_mid_as_screen() before the
        // renegotiated offer hits accept_offer().
        void set_screen_video_rtp_handler (RtpHandler handler) noexcept;
        void send_screen_video_rtp (::rtc::message_variant data) noexcept;

        // marks the given SDP mid as carrying screen-capture video so that
        // onTrack dispatches its RTP to screen_video_rtp_handler instead
        // of video_rtp_handler. safe to call before or after accept_offer.
        void mark_mid_as_screen (std::string mid) noexcept;

        // inverse of mark_mid_as_screen — the m-line stops carrying screen
        // capture (browser turned share off). subsequent RTP on that mid
        // routes back through the camera handler.
        void unmark_mid_as_screen (const std::string& mid) noexcept;

        // true when this peer currently has an open outbound screen track
        // (i.e. it can RECEIVE someone else's screen share).
        [[nodiscard]] bool has_screen_track () const noexcept;

        // ---- SFU-initiated renegotiation (screen-share consumer side) ----
        // Existing flow is browser-initiated only: the browser writes an offer,
        // accept_offer() applies it and reads back the auto-generated answer
        // synchronously. For screen-share viewers we also need the *inverse*
        // direction — the SFU has to grow an outbound screen track on each
        // viewer's PeerConnection at the moment some other peer in the room
        // starts sharing, and then drive a fresh offer/answer cycle initiated
        // from this side.

        // Invoked from libdatachannel's onLocalDescription whenever this peer
        // emits an SDP whose type is Offer (i.e. an SFU-initiated one). The
        // standard Answer path is unaffected — accept_offer() still reads its
        // result synchronously via localDescription() and does NOT route
        // through this handler.
        using RenegotiationOfferHandler = std::function<void(std::string sdp)>;
        void set_renegotiation_offer_handler (RenegotiationOfferHandler handler) noexcept;

        // Adds an outbound screen video track and triggers an SFU-initiated
        // SDP offer cycle. The offer SDP is delivered asynchronously through
        // renegotiation_offer_handler (registered via the setter above), which
        // SfuGrpcService forwards onto the peer-event stream so the gateway
        // can ship it to the browser as Envelope.server_sdp_offer. Returns
        // false if the peer connection isn't initialized, or if a
        // renegotiation is already in flight (glare guard — caller can retry
        // once accept_renegotiation_answer has settled).
        bool start_screen_consumer_renegotiation () noexcept;

        // Applies the browser's SDP answer to a previously-emitted
        // renegotiation offer. Pairs with start_screen_consumer_renegotiation;
        // clears the in-flight flag on success so the next renegotiation can
        // run.
        bool accept_renegotiation_answer (std::string_view sdp_answer) noexcept;

    private:
        std::shared_ptr<::rtc::PeerConnection> peer_connection { }; // libdatachannel peer
        std::shared_ptr<::rtc::Track> video_echo_track { }; // outgoing video; re-emits incoming RTP
        std::shared_ptr<::rtc::Track> audio_echo_track { }; // outgoing audio; re-emits incoming RTP
        std::shared_ptr<::rtc::Track> screen_video_track { }; // outgoing screen video; populated when peer is a *consumer* of someone else's screen share
        std::string cached_answer_sdp { }; // answer SDP from accept_offer
        std::string id_str { }; // peer identifier assigned by the owning Room
        std::atomic<bool> connected { false }; // mirrors PeerConnection::State::Connected
        IceCandidateHandler ice_candidate_handler { }; // optional — pushed on each onLocalCandidate
        PeerReadyHandler peer_ready_handler { }; // optional — pushed once when state transitions to Connected
        RtpHandler audio_rtp_handler { }; // inboud video invoked from onTrack's onMessage
        RtpHandler video_rtp_handler { }; // inbound audio invoked from onTrack's onMessage
        RtpHandler screen_video_rtp_handler { }; // inbound screen video invoked from onTrack's onMessage when track mid is in screen_mids
        RenegotiationOfferHandler renegotiation_offer_handler { }; // invoked when libdatachannel emits a local description of type Offer (SFU-initiated renegotiation)
        std::atomic<bool> renegotiation_in_flight { false }; // glare guard — true between start_screen_consumer_renegotiation and accept_renegotiation_answer
        mutable std::mutex screen_mids_mutex { }; // guards screen_mids — written by gRPC MarkScreenShare thread, read by libdatachannel onTrack thread
        std::unordered_set<std::string> screen_mids { }; // SDP mids the browser has labelled as screen tracks

    };

}

#endif // SFU_PEER_HH
