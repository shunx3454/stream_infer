#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

template <typename _Type> class BUFPOOL {
  public:
    using value_type = _Type;
    using pointer = std::shared_ptr<_Type>;

    explicit BUFPOOL(size_t count) { allocate(count); }

    template <typename... Args, std::enable_if_t<(sizeof...(Args) > 0), int> = 0>
    BUFPOOL(size_t count, Args &&...args) {
        allocate(count, args...);
    }

    BUFPOOL(const BUFPOOL &) = delete;
    BUFPOOL &operator=(const BUFPOOL &) = delete;
    BUFPOOL(BUFPOOL &&) = delete;
    BUFPOOL &operator=(BUFPOOL &&) = delete;

    pointer get(std::atomic<bool> &exited) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this, &exited] { return exited.load() || !available_.empty(); });

        if (exited.load())
            return {};

        pointer buffer = std::move(available_.front());
        available_.pop_front();
        available_set_.erase(buffer.get());
        return buffer;
    }

    bool put(pointer buffer, std::atomic<bool> &exited) {
        if (!buffer)
            return false;

        {
            std::lock_guard lock(mutex_);
            if (exited.load() || !owned_set_.contains(buffer.get()) || available_set_.contains(buffer.get()))
                return false;

            available_set_.insert(buffer.get());
            available_.push_back(std::move(buffer));
        }

        cv_.notify_one();
        return true;
    }

    void wakeup() { cv_.notify_all(); }

    size_t capacity() const { return buffers_.size(); }

    size_t available() const {
        std::lock_guard lock(mutex_);
        return available_.size();
    }

  private:
    template <typename... Args> void allocate(size_t count, Args &&...args) {
        if (count == 0)
            throw std::invalid_argument("BUFPOOL count must be greater than zero");

        buffers_.reserve(count);
        owned_set_.reserve(count);
        available_set_.reserve(count);

        for (size_t i = 0; i < count; ++i) {
            // Named arguments are intentionally reused as lvalues for every buffer.
            pointer buffer = std::make_shared<_Type>(args...);
            owned_set_.insert(buffer.get());
            available_set_.insert(buffer.get());
            available_.push_back(buffer);
            buffers_.push_back(std::move(buffer));
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<pointer> buffers_;
    std::deque<pointer> available_;
    std::unordered_set<_Type *> owned_set_;
    std::unordered_set<_Type *> available_set_;
};
