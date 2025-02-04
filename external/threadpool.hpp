#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <queue>
#include <stdexcept>
#include <thread>

class ThreadPool {
   public:
    using TaskFunction = std::function<void(size_t)>;

    ThreadPool(std::size_t num_threads) : stop_(false) {
        for (std::size_t i = 0; i < num_threads; ++i) {
            workers_.emplace_back([this, i] { work(i); });
        }
    }

    template <class F, class... Args>
    void enqueue(F&& func, Args&&... args) {
        auto task = std::make_shared<std::packaged_task<void(size_t)>>(
            [f = std::forward<F>(func), ... largs = std::forward<Args>(args)](size_t thread_idx) {
                f(thread_idx, std::forward<Args>(largs)...);
            });

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) throw std::runtime_error("Warning: enqueue on stopped ThreadPool");
            tasks_.emplace([task](size_t idx) { (*task)(idx); });
        }
        condition_.notify_one();
    }

    ~ThreadPool() { wait(); }

    void wait() {
        if (stop_) return;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }

        condition_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

   private:
    void work(size_t thread_idx) {
        while (true) {
            TaskFunction task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task(thread_idx);
        }
    }

    std::vector<std::thread> workers_;
    std::queue<TaskFunction> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;

    std::atomic_bool stop_;
};