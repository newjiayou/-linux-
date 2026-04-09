#ifndef REDIS_CONNECTION_POOL_H
#define REDIS_CONNECTION_POOL_H

#include <hiredis/hiredis.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

using RedisConnectionPtr = std::unique_ptr<redisContext, std::function<void(redisContext*)>>;

class RedisConnectionPool {
public:
    static RedisConnectionPool& getInstance();

    void configure(std::string host, int port, int poolSize);
    bool init();
    RedisConnectionPtr getConnection();

private:
    RedisConnectionPool() = default;
    ~RedisConnectionPool();

    RedisConnectionPool(const RedisConnectionPool&) = delete;
    RedisConnectionPool& operator=(const RedisConnectionPool&) = delete;

    void closeAll();

private:
    std::string m_host;
    int m_port = 6379;
    int m_poolSize = 0;

    std::queue<redisContext*> m_connections;
    std::mutex m_mutex;
    std::condition_variable m_cond;
};

#endif // REDIS_CONNECTION_POOL_H
