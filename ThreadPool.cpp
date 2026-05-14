#include "ThreadPool.h"
#include <mysql/mysql.h> // mysql_thread_init/end for per-thread MySQL C API init

ThreadPool::ThreadPool(size_t threads, size_t maxQueueSize) : stop(false), max_queue_size(maxQueueSize) {
    for(size_t i = 0; i < threads; ++i)
        workers.emplace_back([this] {
            // MySQL C API: each native thread should call init/end.
            // Without this, concurrent DB operations may degrade or behave incorrectly.
            mysql_thread_init();
            for(;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                    if(this->stop && this->tasks.empty()) break;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
            mysql_thread_end();
        });
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        tasks.emplace(std::move(task));
    }
    condition.notify_one();
}

bool ThreadPool::try_enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if (stop) return false;
        if (max_queue_size > 0 && tasks.size() >= max_queue_size) {
            return false;
        }
        tasks.emplace(std::move(task));
    }
    condition.notify_one();
    return true;
}

ThreadPool::~ThreadPool() {
    { std::unique_lock<std::mutex> lock(queue_mutex); stop = true; }
    condition.notify_all();
    for(std::thread &worker: workers) worker.join();
}

int ThreadPool::get_size()
{
    return workers.size();
}

size_t ThreadPool::get_queue_size()
{
    std::unique_lock<std::mutex> lock(queue_mutex);
    return tasks.size();
}
