#ifndef SPSC_QUEUE_HH
#define SPSC_QUEUE_HH

// related headers

// c sys headers

// cpp stdlib headers
#include <cstddef>
#include <array>
#include <atomic>
#include <utility>

// 3rd party headers

// project headers

namespace OpenSocialNet::Network
{

    // single-producer, single-consumer lock-free bounded queue.
    // one thread calls try_push, one (different) thread calls try_pop.
    // capacity is fixed at compile time and must be a power of two so we
    // can mask wrap-around with a bitwise AND instead of a modulo.
    template<typename T, std::size_t Capacity>
    class SpscQueue
    {

        static_assert(Capacity >= 2, "Capacity must be at least 2");
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

    public:
        SpscQueue() = default;
        ~SpscQueue() = default;

        SpscQueue(const SpscQueue&) = delete;
        SpscQueue& operator=(const SpscQueue&) = delete;
        SpscQueue(SpscQueue&&) = delete;
        SpscQueue& operator=(SpscQueue&&) = delete;

        // producer side: move value into the slot at tail and publish.
        // returns false if the queue is full.
        bool try_push(T value) noexcept
        {

            const std::size_t cur_tail { tail.load(std::memory_order_relaxed) };
            const std::size_t cur_head { head.load(std::memory_order_acquire) };

            if ((cur_tail - cur_head) >= Capacity) return false;

            samples[cur_tail & (Capacity - 1)] = std::move(value);
            tail.store(cur_tail + 1, std::memory_order_release);
            return true;

        }

        // consumer side: move slot at head into out and advance.
        // returns false if the queue is empty.
        bool try_pop(T& out) noexcept
        {

            const std::size_t cur_head { head.load(std::memory_order_relaxed) };
            const std::size_t cur_tail { tail.load(std::memory_order_acquire) };

            if (cur_head == cur_tail) return false;

            out = std::move(samples[cur_head & (Capacity - 1)]);
            head.store(cur_head + 1, std::memory_order_release);
            return true;

        }

        // best-effort snapshot of the number of in-flight elements.
        // not guaranteed to be exact under concurrent push/pop.
        std::size_t size() const noexcept
        {

            const std::size_t cur_tail { tail.load(std::memory_order_acquire) };
            const std::size_t cur_head { head.load(std::memory_order_acquire) };
            return cur_tail - cur_head;

        }

        // returns true if the queue currently has no elements.
        bool empty() const noexcept { return size() == 0; }

        // returns true if the queue currently has Capacity elements.
        bool full() const noexcept { return size() == Capacity; }

    private:
        // storage for the queued elements. fixed-size, no heap allocation.
        // indexed by (tail | head) & (Capacity - 1) for cheap wrap-around.
        std::array<T, Capacity> samples {};

        // consumer's read cursor. monotonically increasing; masked on access.
        // padded to its own 64-byte cache line so producer writes to `tail`
        // do not invalidate the consumer's cache line (false sharing).
        alignas(64) std::atomic<std::size_t> head { 0 };
        char head_pad[64 - sizeof(std::atomic<std::size_t>)] {}; // padding tail off head's cache line

        // producer's write cursor. monotonically increasing; masked on access.
        // also aligned to its own cache line for the same reason as `head`.
        alignas(64) std::atomic<std::size_t> tail { 0 };
        char tail_pad[64 - sizeof(std::atomic<std::size_t>)] {}; // padding next neighbour off tail's line

    };

}

#endif // SPSC_QUEUE_HH
