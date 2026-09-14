#pragma once

#include <atomic>
#include <cstddef>

namespace proto {

// A lock-free ring buffer for exactly one producer thread and one consumer
// thread (SPSC). The producer only ever calls push(); the consumer only ever
// calls pop(). No mutex is involved: the two sides coordinate through two
// atomic indices and acquire/release ordering.
//
// One slot is always left empty so that "head == tail" means empty and never
// ambiguously means full. A ring_buffer<T, N> therefore holds up to N - 1
// elements.
template <typename T, std::size_t Capacity>
class ring_buffer {
    static_assert(Capacity >= 2, "ring_buffer needs at least two slots");

public:
    ring_buffer() = default;

    ring_buffer(const ring_buffer&) = delete;
    ring_buffer& operator=(const ring_buffer&) = delete;

    // Producer side. Returns false (dropping the value) when the buffer is full.
    bool push(const T& value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = advance(tail);
        if (next == head_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[tail] = value;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false and leaves out untouched when the buffer is
    // empty.
    bool pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buffer_[head];
        head_.store(advance(head), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    bool full() const {
        const std::size_t next = advance(tail_.load(std::memory_order_acquire));
        return next == head_.load(std::memory_order_acquire);
    }

    // Approximate while the other side is running; exact when the buffer is
    // quiescent.
    std::size_t size() const {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return (tail + Capacity - head) % Capacity;
    }

    static constexpr std::size_t capacity() { return Capacity - 1; }

private:
    static constexpr std::size_t advance(std::size_t index) {
        return (index + 1) % Capacity;
    }

    T buffer_[Capacity] = {};
    std::atomic<std::size_t> head_{0};  // next slot to read; owned by the consumer
    std::atomic<std::size_t> tail_{0};  // next slot to write; owned by the producer
};

}  // namespace proto