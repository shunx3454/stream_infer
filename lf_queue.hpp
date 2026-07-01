#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

// Fixed-size bounded MPMC queue.
//
// enqueue_pos_ / dequeue_pos_ are monotonic logical positions. They are not
// wrapped. The real array index is calculated by "pos % capacity".
//
// Each slot owns one sequence number:
//   sequence == enqueue_pos      : this slot is empty and can be written
//   sequence == dequeue_pos + 1  : this slot has valid data and can be read
//   sequence == old value        : this slot still belongs to an older round
//
// This avoids the ABA problem caused by reusing the same ring index after wrap.
template <typename T, size_t capacity> class LFArrayQueue {
  public:
    LFArrayQueue() : enqueue_pos_(0), dequeue_pos_(0) {
        static_assert(capacity > 0, "LFArrayQueue capacity must be greater than zero");
        static_assert(std::is_default_constructible<T>::value,
                      "LFArrayQueue requires default constructible value type");

        // At startup slot i can be written by the producer that owns logical
        // enqueue position i.
        for (size_t i = 0; i < capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~LFArrayQueue() = default;

    LFArrayQueue(const LFArrayQueue &) = delete;
    LFArrayQueue(LFArrayQueue &&) = delete;
    LFArrayQueue &operator=(const LFArrayQueue &) = delete;
    LFArrayQueue &operator=(LFArrayQueue &&) = delete;

    bool push(const T &val) { return push_impl(val); }

    bool push(T &&val) { return push_impl(std::move(val)); }

    bool pop(T &val) {
        Cell *cell = nullptr;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos % capacity];
            size_t seq = cell->sequence.load(std::memory_order_acquire);

            // For a consumer, the expected readable state is pos + 1.
            //
            // diff == 0: this slot contains the data for this dequeue pos.
            // diff < 0 : producer has not published this slot yet, queue empty.
            // diff > 0 : another consumer advanced dequeue_pos_, retry.
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                // Claim this logical read position. After this succeeds, this
                // consumer exclusively owns the slot and may read cell->data.
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        val = std::move(cell->data);

        // Release the slot to the next producer round. Example: capacity = 4,
        // reading pos 0 releases slot 0 for enqueue pos 4.
        cell->sequence.store(pos + capacity, std::memory_order_release);
        return true;
    }

    constexpr size_t capacity_size() const { return capacity; }

  private:
    struct Cell {
        std::atomic<size_t> sequence;
        T data;

        Cell() : sequence(0), data() {}
    };

    template <typename U> bool push_impl(U &&val) {
        Cell *cell = nullptr;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos % capacity];
            size_t seq = cell->sequence.load(std::memory_order_acquire);

            // For a producer, the expected writable state is pos.
            //
            // diff == 0: this slot is free for this enqueue pos.
            // diff < 0 : consumer has not released this slot yet, queue full.
            // diff > 0 : another producer advanced enqueue_pos_, retry.
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                // Claim this logical write position. After this succeeds, this
                // producer exclusively owns the slot and may write cell->data.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        cell->data = std::forward<U>(val);

        // Publish the data. The acquire load in pop() pairs with this release
        // store, so the consumer sees cell->data after sequence becomes pos + 1.
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    std::array<Cell, capacity> buffer_;

    // Next logical position to be claimed by producers. This grows forever and
    // is mapped to a slot by "enqueue_pos_ % capacity".
    std::atomic<size_t> enqueue_pos_;

    // Next logical position to be claimed by consumers. This grows forever and
    // is mapped to a slot by "dequeue_pos_ % capacity".
    std::atomic<size_t> dequeue_pos_;
};
