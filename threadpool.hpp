#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
  public:
    using WorkerHook = std::function<void(size_t)>;

    explicit ThreadPool(size_t thread_count) : ThreadPool(thread_count, {}, {}) {}

    ThreadPool(size_t thread_count, WorkerHook on_worker_start, WorkerHook on_worker_stop = {})
        : on_worker_start_(std::move(on_worker_start)), on_worker_stop_(std::move(on_worker_stop)) {
        if (thread_count == 0)
            throw std::invalid_argument("ThreadPool thread_count must be greater than zero");

        workers_.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this, i]() { workerLoop(i); });
        }
    }

    ~ThreadPool() { stop(true); }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool(ThreadPool &&) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool &operator=(ThreadPool &&) = delete;

    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
        using Ret = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<Ret()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stopping_)
                throw std::runtime_error("ThreadPool is stopped");
            tasks_.emplace_back([task]() { (*task)(); });
        }

        cv_.notify_one();
        return future;
    }

    void stop(bool drain = true) {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!stopping_) {
                stopping_ = true;
                drain_on_stop_ = drain;
                notify = true;

                if (!drain_on_stop_)
                    tasks_.clear();
            }
        }

        if (notify)
            cv_.notify_all();

        for (auto &worker : workers_) {
            if (worker.joinable())
                worker.join();
        }
    }

    size_t size() const { return workers_.size(); }

    size_t pending() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return tasks_.size();
    }

    bool stopped() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return stopping_;
    }

    static size_t currentWorkerIndex() { return current_worker_index_; }

    static bool isPoolWorker() { return current_worker_index_ != kInvalidWorkerIndex; }

  private:
    static constexpr size_t kInvalidWorkerIndex = static_cast<size_t>(-1);
    inline static thread_local size_t current_worker_index_ = kInvalidWorkerIndex;

    void workerLoop(size_t worker_index) {
        current_worker_index_ = worker_index;

        if (on_worker_start_)
            on_worker_start_(worker_index);

        for (;;) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });

                if (stopping_ && (!drain_on_stop_ || tasks_.empty()))
                    break;

                task = std::move(tasks_.front());
                tasks_.pop_front();
            }

            task();
        }

        if (on_worker_stop_)
            on_worker_stop_(worker_index);

        current_worker_index_ = kInvalidWorkerIndex;
    }

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    WorkerHook on_worker_start_;
    WorkerHook on_worker_stop_;
    bool stopping_ = false;
    bool drain_on_stop_ = true;
};
