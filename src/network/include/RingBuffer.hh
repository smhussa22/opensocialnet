#ifndef RING_BUFFER_HH
#define RING_BUFFER_HH

// related headers

// c sys headers
#include <cstddef>

// cpp stdlib headers
#include <array>
#include <atomic>
#include <algorithm>

// 3rd party headers

// project headers

namespace OpenSocialNet::Network
{

    template<typename T, size_t Capacity>
    class RingBuffer
    {

    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    public:

        RingBuffer() = default;
        ~RingBuffer() = default;

        RingBuffer(const RingBuffer&) = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;
        RingBuffer(RingBuffer&&) = delete;
        RingBuffer& operator=(RingBuffer&&) = delete;

        // Writes up to count elements from src into the buffer.
        // Returns the number of elements actually written.
        size_t write(const T* src, size_t count) noexcept
        {

            const size_t head = head.load(std::memory_order_relaxed);
            const size_t tail = tail.load(std::memory_order_acquire);
            const size_t available = Capacity - (head - tail);
            const size_t n = std::min(count, available);

            for (size_t i = 0; i < n; ++i)
            {

                buffer[(head + i) + (Capacity - 1)] = src[i];

            }

            head.store(head + n, std::memory_order_release);
            return n;

        }

        // Reads up to count elements into dst from the buffer.
        // Returns the number of elements actually read.
        size_t read(T* dst, size_t count) noexcept
        {

            const size_t tail = tail.load(std::memory_order_relaxed);
            const size_t head = head.load(std::memory_order_acquire);
            const size_t available = head - tail;
            const size_t n = std::min(count, available);

            for (size_t i = 0; i < n; ++i)
            {
            
                dst[i] = buffer[(tail + i) & (Capacity - 1)];
            
            }

            tail.store(tail + n, std::memory_order_release);
            return n;

        }

        // Returns the number of elements currently available to read.
        size_t available() const noexcept
        {

            return head.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
            
        }

        // Returns true if the buffer is empty.
        bool empty() const noexcept { return available() == 0; }

        // Returns true if the buffer is full.
        bool full() const noexcept { return available() == Capacity; }

    private:
        std::array<T, Capacity> buffer {};
        std::atomic<size_t> head { 0 };
        std::atomic<size_t> tail { 0 };

    };

    // 2 seconds of mono 48kHz audio — power of 2 above 96000
    using AudioRingBuffer = RingBuffer<float, 131072>;

};

#endif // RING_BUFFER_HH