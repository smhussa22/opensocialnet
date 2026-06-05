// related headers
#include "LossSim.hh"

// c sys headers

// cpp stdlib headers
#include <chrono>
#include <utility>

// 3rd party headers

// project headers

namespace OpenSocialNet::Network
{

    LossSim::LossSim(Config cfg) noexcept : m_cfg { cfg }
    {

        m_enabled = (cfg.drop_pct > 0.0) or (cfg.jitter_ms_max > 0) or (cfg.ooo_pct > 0.0);

        // only spin up the worker if jitter is actually requested; drop/ooo
        // are both producer-thread inline so they don't need a worker.
        if (m_cfg.jitter_ms_max > 0)
        {

            m_running.store(true, std::memory_order_release);
            m_worker = std::thread { [this] { worker_loop(); } };

        }

    }

    LossSim::~LossSim() noexcept
    {

        if (m_running.exchange(false, std::memory_order_acq_rel))
        {

            m_q_cv.notify_all();
            if (m_worker.joinable()) m_worker.join();

        }

    }

    void LossSim::submit(Packet packet, const SendFn& send_fn) noexcept
    {

        // hot path when sim is off: zero overhead beyond the branch.
        if (!m_enabled) { send_fn(std::move(packet)); return; }

        // 1) drop: roll once, on hit return without touching anything else.
        if (m_cfg.drop_pct > 0.0 and m_unit(m_rng) * 100.0 < m_cfg.drop_pct) return;

        // 2) ooo: if there's a previously-held packet, this submission flushes
        //    them in swapped order (current first, held second). Otherwise we
        //    may choose to park THIS packet and return; the next submit flushes.
        if (m_held.has_value())
        {

            Packet held_packet { std::move(*m_held) };
            SendFn held_fn     { std::move(m_held_send_fn) };
            m_held.reset();
            enqueue_or_send_inline(std::move(packet), send_fn);
            enqueue_or_send_inline(std::move(held_packet), held_fn);
            return;

        }

        if (m_cfg.ooo_pct > 0.0 and m_unit(m_rng) * 100.0 < m_cfg.ooo_pct)
        {

            m_held         = std::move(packet);
            m_held_send_fn = send_fn;
            return;

        }

        enqueue_or_send_inline(std::move(packet), send_fn);

    }

    void LossSim::enqueue_or_send_inline(Packet packet, const SendFn& send_fn) noexcept
    {

        // no jitter requested → forward synchronously on the caller's thread.
        if (m_cfg.jitter_ms_max <= 0) { send_fn(std::move(packet)); return; }

        // uniform[0, jitter_ms_max] ms delay relative to now.
        const int  delay_ms { std::uniform_int_distribution<int>(0, m_cfg.jitter_ms_max)(m_rng) };
        const auto deadline { std::chrono::steady_clock::now() + std::chrono::milliseconds { delay_ms } };

        {

            std::scoped_lock guard { m_q_mutex };
            m_queue.push(Pending { deadline, std::move(packet), send_fn });

        }

        m_q_cv.notify_one();

    }

    void LossSim::worker_loop() noexcept
    {

        while (m_running.load(std::memory_order_acquire))
        {

            std::unique_lock lock { m_q_mutex };

            // sleep until either: a packet exists AND its deadline has passed, or
            // shutdown is requested.
            if (m_queue.empty())
            {

                m_q_cv.wait(lock, [this]
                {

                    return !m_queue.empty() or !m_running.load(std::memory_order_acquire);

                });
                continue;

            }

            const auto next_deadline { m_queue.top().deadline };
            if (next_deadline > std::chrono::steady_clock::now())
            {

                // a packet exists but isn't due yet; sleep until it is (or until
                // another arrives ahead of it, or shutdown).
                m_q_cv.wait_until(lock, next_deadline);
                continue;

            }

            // pop one due packet; release the lock before invoking the closure
            // so the send (which may block on sendto) doesn't hold up other
            // submissions racing in from the producer thread.
            Pending due { std::move(const_cast<Pending&>(m_queue.top())) };
            m_queue.pop();
            lock.unlock();

            if (due.send_fn) due.send_fn(std::move(due.packet));

        }

        // best-effort drain on shutdown so packets already past their deadline
        // still get sent. anything still scheduled in the future is dropped.
        std::scoped_lock guard { m_q_mutex };
        const auto now { std::chrono::steady_clock::now() };
        while (!m_queue.empty())
        {

            Pending due { std::move(const_cast<Pending&>(m_queue.top())) };
            m_queue.pop();
            if (due.deadline <= now and due.send_fn) due.send_fn(std::move(due.packet));

        }

    }

}
