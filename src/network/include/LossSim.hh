#ifndef LOSS_SIM_HH
#define LOSS_SIM_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <thread>

// 3rd party headers

// project headers
#include "Packet.hh"

namespace OpenSocialNet::Network
{

    // Sender-side adversarial-condition injector for the native UDP transport.
    //
    // Knobs (all bypassed when zero — submit() is then a tail call to send_fn):
    //   drop_pct      0..100  : packet silently dropped this often
    //   jitter_ms_max 0..N    : forwarded packets delayed by uniform[0, max] ms
    //                           on an internal worker thread
    //   ooo_pct       0..100  : packet held; the next submit() flushes them
    //                           in swapped order so the receiver actually sees
    //                           seq=N+1 before seq=N (requires capture-time
    //                           stamping; see UdpSender::stamp)
    //
    // Threading: submit() is intended to be called from one producer thread.
    // The internal worker (only spawned when jitter_ms_max > 0) invokes the
    // SendFn closure from its own thread, so the closure's targets (e.g. the
    // UdpSender's send_raw) must tolerate calls from either the producer
    // thread (zero-jitter path) or the worker (jitter path) — but never both
    // concurrently for the same packet.
    class LossSim
    {

    public:

        using SendFn = std::function<void(Packet)>;

        struct Config
        {

            double drop_pct      { 0.0 };  // % of submissions silently dropped
            int    jitter_ms_max { 0   };  // max added delay per packet, milliseconds
            double ooo_pct       { 0.0 };  // % of submissions held to swap with next

        };

        explicit LossSim(Config cfg) noexcept;
        ~LossSim() noexcept;

        LossSim(const LossSim&)            = delete;
        LossSim& operator=(const LossSim&) = delete;
        LossSim(LossSim&&)                 = delete;
        LossSim& operator=(LossSim&&)      = delete;

        // Apply drop / ooo / jitter policies to packet, then forward via send_fn.
        // If all knobs are zero this is a synchronous forward — no allocation,
        // no thread hop. Otherwise the closure may be invoked either inline or
        // on the worker thread, depending on the jitter draw.
        void submit(Packet packet, const SendFn& send_fn) noexcept;

        bool enabled() const noexcept { return m_enabled; }   // true if any knob is non-zero
        const Config& config() const noexcept { return m_cfg; }

    private:

        // priority_queue entry: deadline first so the heap pops earliest first.
        struct Pending
        {

            std::chrono::steady_clock::time_point deadline {};  // when to actually send
            Packet  packet  {};
            SendFn  send_fn {};

            // priority_queue is a max-heap by default; invert so the *earliest*
            // deadline sits at the top.
            bool operator<(const Pending& other) const noexcept { return deadline > other.deadline; }

        };

        // Either schedules the packet for delayed send, or forwards inline if
        // jitter is disabled. The OOO branch in submit() calls into here once
        // it has decided what to release.
        void enqueue_or_send_inline(Packet packet, const SendFn& send_fn) noexcept;

        // Worker thread body: blocks on m_q_cv until either the next pending
        // packet is due or shutdown is requested.
        void worker_loop() noexcept;

        Config m_cfg     {};       // captured at construction; immutable
        bool   m_enabled { false };// any knob > 0 ⇒ submit() does non-trivial work

        std::mt19937                            m_rng  { std::random_device{}() }; // shared PRNG, producer-thread only
        std::uniform_real_distribution<double>  m_unit { 0.0, 1.0 };               // uniform[0,1) draw for drop/ooo

        // OOO state — touched only by the producer thread, so no synchronisation.
        std::optional<Packet> m_held         {};   // packet parked for swap-with-next
        SendFn                m_held_send_fn {};   // its destination closure

        // Jitter worker.
        std::mutex                   m_q_mutex {};                 // protects m_queue
        std::condition_variable      m_q_cv    {};                 // wakes worker on new pending / shutdown
        std::priority_queue<Pending> m_queue   {};                 // earliest deadline at top
        std::atomic<bool>            m_running { false };          // worker liveness flag
        std::thread                  m_worker  {};                 // only joined in dtor

    };

}

#endif // LOSS_SIM_HH
