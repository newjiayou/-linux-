#include "RedisConnectionPool.h"

#include <iostream>

RedisConnectionPool& RedisConnectionPool::getInstance() {
    static RedisConnectionPool instance;
    return instance;
}

void RedisConnectionPool::configure(std::string host, int port, int poolSize) {
    m_host = std::move(host);
    m_port = port;
    m_poolSize = poolSize;
}

bool RedisConnectionPool::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int i = 0; i < m_poolSize; ++i) {
        redisContext* conn = redisConnect(m_host.c_str(), m_port);
        if (!conn || conn->err) {
            std::cerr << "[REDIS_POOL] Error: redisConnect failed";
            if (conn && conn->errstr) {
                std::cerr << ": " << conn->errstr;
            }
            std::cerr << std::endl;
            if (conn) {
                redisFree(conn);
            }
            closeAll();
            return false;
        }
        m_connections.push(conn);
    }
    return true;
}

RedisConnectionPtr RedisConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cond.wait(lock, [this] { return !m_connections.empty(); });

    redisContext* conn = m_connections.front();
    m_connections.pop();

    return RedisConnectionPtr(conn, [this](redisContext* releasedConn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.push(releasedConn);
        m_cond.notify_one();
    });
}

RedisConnectionPool::~RedisConnectionPool() {
    closeAll();
}

void RedisConnectionPool::closeAll() {
    while (!m_connections.empty()) {
        redisFree(m_connections.front());
        m_connections.pop();
    }
}
