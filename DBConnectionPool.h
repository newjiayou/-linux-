#ifndef DB_CONNECTION_POOL_H
#define DB_CONNECTION_POOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <functional>
#include <iostream>

// RAII 包装器，使用 std::unique_ptr 和自定义 deleter 实现
// 这样可以保证连接在使用完毕后自动返回池中
using ConnectionPtr = std::unique_ptr<MYSQL, std::function<void(MYSQL*)>>;

class DBConnectionPool {
public:
    // 获取单例
    static DBConnectionPool& getInstance();

    // 配置连接池参数
    void configure(std::string host, std::string user, std::string password, std::string dbname, int port, int poolSize);
    
    // 初始化连接池
    bool init();

    // 从池中获取一个连接
    ConnectionPtr getConnection();

private:
    // 私有构造和析构，确保单例模式
    DBConnectionPool() = default;
    ~DBConnectionPool();

    // 禁止拷贝和赋值
    DBConnectionPool(const DBConnectionPool&) = delete;
    DBConnectionPool& operator=(const DBConnectionPool&) = delete;
    
    // 释放所有连接
    void closeAll();

private:
    std::string m_host;
    std::string m_user;
    std::string m_password;
    std::string m_dbname;
    int m_port;
    int m_poolSize;

    std::queue<MYSQL*> m_connections; // 连接队列
    std::mutex m_mutex;
    std::condition_variable m_cond;
};

#endif // DB_CONNECTION_POOL_H