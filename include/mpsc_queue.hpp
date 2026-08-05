// mpsc_queue.hpp -- a bounded, lock-free multi-producer / single-consumer queue.
//
// This is Dmitry Vyukov's bounded MPMC ring (correct, and used here as MPSC):
// every cell carries a sequence number that sequences producers and the
// consumer without a lock. Producers claim a slot with a single CAS on
// enqueue_pos_; the lone consumer advances dequeue_pos_. Capacity is fixed at
// construction and must be a power of two, so `& mask_` replaces a modulo.
//
// It is intentionally a queue of trivially-copyable values (we push small POD
// request structs), so there is no allocation on the hot path and no
// memory-reclamation problem to reason about.
//
// enqueue() returns false when the ring is full; try_dequeue() returns false
// when it is empty. Neither ever blocks.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

template <typename T>
class MpscQueue
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "MpscQueue holds trivially-copyable payloads only");

    struct Cell
    {
        std::atomic<std::size_t> seq;
        T data;
    };

    // Avoid false sharing between the producer-hot and consumer-hot counters.
    static constexpr std::size_t kCacheLine = 64;

public:
    // capacity is rounded up to a power of two, minimum 2.
    explicit MpscQueue(std::size_t capacity)
    {
        std::size_t cap = 2;
        while (cap < capacity) cap <<= 1;
        mask_ = cap - 1;
        buffer_ = new Cell[cap];
        for (std::size_t i = 0; i < cap; ++i)
            buffer_[i].seq.store(i, std::memory_order_relaxed);
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    ~MpscQueue() { delete[] buffer_; }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    // Producer side. Safe to call from many threads. Returns false if full.
    bool enqueue(const T& value)
    {
        Cell* cell;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            std::intptr_t dif =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (dif == 0)
            {
                // Cell is free and it is our turn to claim `pos`.
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
                // lost the race -> pos was reloaded, retry
            }
            else if (dif < 0)
            {
                return false; // queue full
            }
            else
            {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = value;
        cell->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Single-consumer only. Returns false if empty.
    bool try_dequeue(T& out)
    {
        Cell* cell;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;)
        {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->seq.load(std::memory_order_acquire);
            std::intptr_t dif = static_cast<std::intptr_t>(seq) -
                                static_cast<std::intptr_t>(pos + 1);
            if (dif == 0)
            {
                if (dequeue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (dif < 0)
            {
                return false; // queue empty
            }
            else
            {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = cell->data;
        cell->seq.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

private:
    Cell* buffer_ = nullptr;
    std::size_t mask_ = 0;
    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_;
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_;
};
