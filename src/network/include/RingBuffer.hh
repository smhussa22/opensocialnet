#ifndef RING_BUFFER_HH
#define RING_BUFFER_HH

#include <cstddef>
#include <array>
#include <atomic>
#include <algorithm>

namespace OpenSocialNet::Audio
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

            const size_t write_pos = write_index.load(std::memory_order_relaxed);
            const size_t read_pos = read_index.load(std::memory_order_acquire);
            const size_t space_left = Capacity - (write_pos - read_pos);
            const size_t n  = std::min(count, space_left);

            for (size_t i = 0; i < n; ++i) samples[(write_pos + i) & (Capacity - 1)] = src[i];

            write_index.store(write_pos + n, std::memory_order_release);
            return n;
        }

        // Reads up to count elements into dst from the buffer.
        // Returns the number of elements actually read.
        size_t read(T* dst, size_t count) noexcept
        {

            const size_t read_pos = read_index.load(std::memory_order_relaxed);
            const size_t write_pos = write_index.load(std::memory_order_acquire);
            const size_t samples_ready = write_pos - read_pos;
            const size_t n = std::min(count, samples_ready);

            for (size_t i = 0; i < n; ++i) dst[i] = samples[(read_pos + i) & (Capacity - 1)];

            read_index.store(read_pos + n, std::memory_order_release);
            return n;

        }

        // Returns the number of elements currently available to read.
        size_t available() const noexcept
        {

            return write_index.load(std::memory_order_acquire) - read_index.load(std::memory_order_acquire);

        }

        // Returns true if the buffer is empty.
        bool empty() const noexcept { return available() == 0; }

        // Returns true if the buffer is full.
        bool full() const noexcept { return available() == Capacity; }

    private:
        std::array<T, Capacity> samples {};
        std::atomic<size_t>     write_index { 0 };
        std::atomic<size_t>     read_index  { 0 };

    };

    using AudioRingBuffer = RingBuffer<float, 131072>;

}

#endif // RING_BUFFER_HH