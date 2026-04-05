#include "ThreadPool.h"
#include <mysql/mysql.h> // mysql_thread_init/end for per-thread MySQL C API init

ThreadPool::ThreadPool(size_t threads) : stop(false) {
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
                    if(this->stop && this->tasks.empty()) return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task();
            }
            mysql_thread_end(); // reached only if the loop exits via return
        });
}

void ThreadPool::enqueue(std::function<void()> task) {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        tasks.emplace(std::move(task));
    }
    condition.notify_one();
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