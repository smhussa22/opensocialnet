#ifndef SFU_PEER_HH
#define SFU_PEER_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <atomic>
#include <memory>
#include <string>
#include <string_view>

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

    private:
        std::shared_ptr<::rtc::PeerConnection> peer_connection { }; // libdatachannel peer
        std::shared_ptr<::rtc::Track> video_echo_track { }; // outgoing video; re-emits incoming RTP
        std::shared_ptr<::rtc::Track> audio_echo_track { }; // outgoing audio; re-emits incoming RTP
        std::string cached_answer_sdp { }; // answer SDP from accept_offer
        std::string id_str { }; // peer identifier assigned by the owning Room
        std::atomic<bool> connected { false }; // mirrors PeerConnection::State::Connected

    };

}

#endif // SFU_PEER_HH
