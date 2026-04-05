#include "DBConnectionPool.h"

DBConnectionPool& DBConnectionPool::getInstance() {
    static DBConnectionPool instance;
    return instance;
}

void DBConnectionPool::configure(std::string host, std::string user, std::string password, std::string dbname, int port, int poolSize) {
    m_host = host;
    m_user = user;
    m_password = password;
    m_dbname = dbname;
    m_port = port;
    m_poolSize = poolSize;
}

bool DBConnectionPool::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int i = 0; i < m_poolSize; ++i) {
        MYSQL* conn = mysql_init(NULL);
        if (!conn) {
            std::cerr << "[DB_POOL] Error: mysql_init failed" << std::endl;
            closeAll();
            return false;
        }
        if (!mysql_real_connect(conn, m_host.c_str(), m_user.c_str(), m_password.c_str(), m_dbname.c_str(), m_port, NULL, 0)) {
            std::cerr << "[DB_POOL] Error: mysql_real_connect failed: " << mysql_error(conn) << std::endl;
            mysql_close(conn);
            closeAll();
            return false;
        }
        m_connections.push(conn);
    }
    return true;
}

ConnectionPtr DBConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(m_mutex);
    // 如果池为空，则等待直到有连接可用
    m_cond.wait(lock, [this] { return !m_connections.empty(); });

    MYSQL* conn = m_connections.front();
    m_connections.pop();

    // 使用自定义 deleter 创建 unique_ptr
    // 当 unique_ptr 销毁时，这个 lambda 会被调用，将连接归还给池
    return ConnectionPtr(conn, [this](MYSQL* releasedConn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.push(releasedConn);
        m_cond.notify_one(); // 通知一个等待的线程
    });
}

DBConnectionPool::~DBConnectionPool() {
    closeAll();
}

void DBConnectionPool::closeAll() {
    while (!m_connections.empty()) {
        mysql_close(m_connections.front());
        m_connections.pop();
    }
}