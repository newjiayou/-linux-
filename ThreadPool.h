#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

class ThreadPool {
public:
    ThreadPool(size_t threads, size_t maxQueueSize = 0);
    ~ThreadPool();
    void enqueue(std::function<void()> task);
    bool try_enqueue(std::function<void()> task);
    int get_size();
    size_t get_queue_size();

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    size_t max_queue_size;
};

#endif
