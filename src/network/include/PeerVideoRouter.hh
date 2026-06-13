#ifndef PEER_VIDEO_ROUTER_HH
#define PEER_VIDEO_ROUTER_HH

// related headers

// c sys headers
#include <cstdint>

// cpp stdlib headers
#include <chrono>
#include <cstddef>
#include <memory>
#include <unordered_map>

// 3rd party headers

// project headers
#include "Packet.hh"
#include "PacketJitterBuffer.hh"
#include "VideoDecoder.hh"
#include "VideoPlc.hh"
#include "VideoReassembler.hh"
#include "VideoRenderer.hh"

namespace OpenSocialNet::Network
{

    // Aggregate of every active video source's counters for the single
    // [vid-stats] line.
    struct VideoRouterStats
    {

        std::uint64_t packets_observed     { 0 }; // sum of JitterStats packets_observed across sources
        std::uint64_t packets_lost         { 0 }; // sum of gap-detected losses across sources
        std::uint64_t packets_out_of_order { 0 }; // sum of out-of-order arrivals across sources
        std::uint64_t frames_decoded       { 0 }; // complete frames that made it through the decoder
        std::uint64_t frames_concealed     { 0 }; // frames the per-source VideoPlc synthesised
        std::uint64_t frames_dropped       { 0 }; // frames abandoned with missing fragments
        std::uint64_t frames_missed        { 0 }; // whole frames that vanished (every fragment lost)
        std::uint64_t frames_rendered      { 0 }; // frames actually presented to a window
        std::size_t   buffered_packets     { 0 }; // sum of packets parked in all jitter buffers
        std::size_t   sources              { 0 }; // live source count (peers actively sending video)

    };


    // The multi-peer video receive spine, mirroring PeerMixer's shape:
    // every inbound H264 Packet is demuxed by (peer_id, ssrc) into that
    // source's own PacketJitterBuffer (fragment reorder) +
    // VideoReassembler (fragments -> complete H.264 frame) + VideoDecoder
    // (per-stream reference state) + VideoPlc (loss concealment) +
    // VideoRenderer (one window per remote stream).
    //
    // Threading: unlike PeerMixer this runs entirely on the main loop —
    // route() drains the video SPSC queue and pump() decodes + renders,
    // both from the same thread (SDL windows must be driven from the main
    // thread anyway), so no mutex is needed.
    class PeerVideoRouter
    {

    public:

        explicit PeerVideoRouter(Plc::VideoPlcStrategy plc_strategy, bool render_enabled) noexcept;

        PeerVideoRouter(const PeerVideoRouter&)            = delete;
        PeerVideoRouter& operator=(const PeerVideoRouter&) = delete;
        PeerVideoRouter(PeerVideoRouter&&)                 = delete;
        PeerVideoRouter& operator=(PeerVideoRouter&&)      = delete;


        // Push one inbound packet into its source's jitter buffer,
        // creating the source on first sight (auto-join, same as
        // PeerMixer). Refreshes the source's last-seen time for reap_idle.
        void route(Packet& packet);

        // Drain every source's jitter buffer: reassemble fragments into
        // complete H.264 frames, decode + render them, and conceal frames
        // detected as lost (a fragment gap abandons the partial frame and
        // asks the source's VideoPlc for a substitute). Returns the number
        // of frames presented this call.
        std::size_t pump();

        // Drop sources that haven't sent for `ttl` — their peer left or
        // stopped their camera. Frees the decoder and closes the window.
        std::size_t reap_idle(std::chrono::seconds ttl);

        // Aggregate counters across all live sources for [vid-stats].
        VideoRouterStats stats() const;

        std::size_t source_count() const;


    private:

        // Everything one remote video stream needs on the receive side.
        // Heap allocated because PacketJitterBuffer / VideoPlc /
        // VideoRenderer are pinned (non-movable) types.
        struct Source
        {

            PacketJitterBuffer                    jitter_buffer     { };       // per-source fragment reorder
            Video::VideoReassembler               reassembler       { };       // fragments -> complete H.264 frame
            Video::VideoDecoder                   decoder           { };       // per-stream H.264 reference state
            Plc::VideoPlc                         plc;                         // gap concealment fed from decoded frames
            Video::VideoRenderer                  renderer          { };       // one window per remote stream, opened lazily
            std::uint32_t                         peer_id           { 0 };     // for the window title + logs
            std::uint32_t                         ssrc              { 0 };     // for the window title + logs
            bool                                  is_screen         { false }; // H264_Screen stream — labelled in the window title
            bool                                  renderer_started  { false }; // window opened (needs first frame's dimensions)
            bool                                  renderer_dead     { false }; // window init failed once — stop retrying
            std::uint32_t                         current_timestamp { 0 };     // timestamp of the frame being reassembled
            bool                                  have_timestamp    { false }; // current_timestamp is meaningful
            bool                                  frame_completed   { true };  // last frame finished cleanly (marker seen)
            std::uint32_t                         timestamp_step    { 0 };     // learned per-frame timestamp delta (0 = unknown)
            std::uint16_t                         last_sequence     { 0 };     // seq of the most recently popped packet
            bool                                  have_sequence     { false }; // last_sequence is meaningful
            std::uint64_t                         frames_decoded    { 0 };     // complete frames decoded
            std::uint64_t                         frames_dropped    { 0 };     // frames abandoned with missing fragments
            std::uint64_t                         frames_missed     { 0 };     // whole frames lost with no surviving fragment
            std::uint64_t                         frames_rendered   { 0 };     // frames presented to the window
            std::chrono::steady_clock::time_point last_packet       { };       // refreshed per packet; drives reap_idle

            explicit Source(Plc::VideoPlcStrategy strategy) noexcept : plc { strategy }
            {

                decoder.init();

            }

        };


        // Ask the source's PLC for a substitute frame and present it.
        void conceal_one(Source& source);

        // Lazily open the source's window (first frame supplies the
        // dimensions), then hand the planes to SDL.
        void present(Source& source, const std::uint8_t* const planes[3], const int strides[3], int width, int height);


        Plc::VideoPlcStrategy m_plc_strategy  { Plc::VideoPlcStrategy::Hold }; // strategy stamped into every new source's VideoPlc
        bool                  m_render_enabled { true };                       // false = decode + conceal but never open windows (headless)

        // (peer_id << 32 | ssrc) -> per-source receive state.
        std::unordered_map<std::uint64_t, std::unique_ptr<Source>> m_sources { };

    };

}

#endif // PEER_VIDEO_ROUTER_HH
