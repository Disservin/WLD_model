#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <queue>
#include <stdexcept>
#include <thread>

class ThreadPool {
   public:
    ThreadPool(std::size_t num_threads) : stop_(false) {
        for (std::size_t i = 0; i < num_threads; ++i) workers_.emplace_back([this] { work(); });
    }

    template <class F, class... Args>
    void enqueue(F &&func, Args &&...args) {
        auto task = std::make_shared<std::packaged_task<void()>>(
            [=]() { func(std::forward<Args>(args)..., getThreadId()); });

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) throw std::runtime_error("Warning: enqueue on stopped ThreadPool");
            tasks_.emplace([task]() { (*task)(); });
        }
        condition_.notify_one();
    }

    std::size_t getThreadId() {
        std::thread::id this_id = std::this_thread::get_id();
        std::size_t thread_id = 0;
        for (std::size_t i = 0; i < workers_.size(); ++i) {
            if (workers_[i].get_id() == this_id) {
                thread_id = i;
                break;
            }
        }
        return thread_id;
    }

    ~ThreadPool() { wait(); }

    void wait() {
        if (stop_) return;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }

        condition_.notify_all();

        for (auto &worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

   private:
    void work() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex queue_mutex_;
    std::condition_variable condition_;

    std::atomic_bool stop_;
};
