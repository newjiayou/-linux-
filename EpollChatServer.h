#ifndef EPOLL_CHAT_SERVER_H
#define EPOLL_CHAT_SERVER_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "ThreadPool.h"
#include <memory>
#include <mutex>
#include <mysql/mysql.h> // 新增：MySQL 头文件
#include <hiredis/hiredis.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <atomic> 
#include <cstdlib>
#include <cstring>
#include "DBConnectionPool.h" // 新增：包含连接池头文件
#include "RedisConnectionPool.h"
#include "MemoryPool.h"

class PoolSendBuffer {
public:
    explicit PoolSendBuffer(size_t initialCapacity = 4096)
        : m_capacity(initialCapacity), m_data(static_cast<uint8_t*>(Kama_memoryPool::MemoryPool::allocate(initialCapacity))) {}

    ~PoolSendBuffer() {
        if (m_data) {
            Kama_memoryPool::MemoryPool::deallocate(m_data, m_capacity);
        }
    }

    PoolSendBuffer(const PoolSendBuffer&) = delete;
    PoolSendBuffer& operator=(const PoolSendBuffer&) = delete;

    void append(const void* src, size_t len) {
        if (len == 0) return;
        ensureCapacityForAppend(len);
        memcpy(m_data + m_size, src, len);
        m_size += len;
    }

    const uint8_t* currentData() const { return m_data + m_offset; }
    size_t remaining() const { return m_size - m_offset; }
    size_t pendingAfterAppend(size_t appendLen) const { return remaining() + appendLen; }

    void consume(size_t len) {
        m_offset += len;
        if (m_offset >= m_size) {
            m_offset = 0;
            m_size = 0;
        }
    }

private:
    void ensureCapacityForAppend(size_t appendLen) {
        // 先尝试整理已发送部分，减少扩容次数
        if (m_offset > 0) {
            size_t remain = m_size - m_offset;
            if (remain > 0) {
                memmove(m_data, m_data + m_offset, remain);
            }
            m_size = remain;
            m_offset = 0;
        }

        size_t required = m_size + appendLen;
        if (required <= m_capacity) return;

        size_t newCapacity = m_capacity;
        while (newCapacity < required) newCapacity *= 2;

        uint8_t* newData = static_cast<uint8_t*>(Kama_memoryPool::MemoryPool::allocate(newCapacity));
        memcpy(newData, m_data, m_size);
        Kama_memoryPool::MemoryPool::deallocate(m_data, m_capacity);

        m_data = newData;
        m_capacity = newCapacity;
    }

private:
    size_t m_capacity;
    size_t m_size = 0;
    size_t m_offset = 0;
    uint8_t* m_data;
};

class PoolRecvBuffer {
public:
    explicit PoolRecvBuffer(size_t initialCapacity = 4096)
        : m_capacity(initialCapacity), m_data(static_cast<uint8_t*>(Kama_memoryPool::MemoryPool::allocate(initialCapacity))) {}

    ~PoolRecvBuffer() {
        if (m_data) {
            Kama_memoryPool::MemoryPool::deallocate(m_data, m_capacity);
        }
    }

    PoolRecvBuffer(const PoolRecvBuffer&) = delete;
    PoolRecvBuffer& operator=(const PoolRecvBuffer&) = delete;

    void append(const void* src, size_t len) {
        if (len == 0) return;
        ensureCapacityForAppend(len);
        memcpy(m_data + m_size, src, len);
        m_size += len;
    }

    const uint8_t* data() const { return m_data; }
    size_t size() const { return m_size; }

    void consume(size_t len) {
        if (len >= m_size) {
            m_size = 0;
            return;
        }

        size_t remain = m_size - len;
        memmove(m_data, m_data + len, remain);
        m_size = remain;
    }

private:
    void ensureCapacityForAppend(size_t appendLen) {
        size_t required = m_size + appendLen;
        if (required <= m_capacity) return;

        size_t newCapacity = m_capacity;
        while (newCapacity < required) newCapacity *= 2;

        uint8_t* newData = static_cast<uint8_t*>(Kama_memoryPool::MemoryPool::allocate(newCapacity));
        memcpy(newData, m_data, m_size);
        Kama_memoryPool::MemoryPool::deallocate(m_data, m_capacity);

        m_data = newData;
        m_capacity = newCapacity;
    }

private:
    size_t m_capacity;
    size_t m_size = 0;
    uint8_t* m_data;
};
// 客户端连接状态上下文
struct ClientContext {
    int fd;
    int reactorEpollFd = -1;
    std::string ip;
    PoolRecvBuffer buffer; // 处理粘包的缓冲区
    std::string accountID;
    std::mutex clientMutex;  
    std::mutex sendMutex; 
    // 用户态发送缓冲：当内核 socket 发送缓冲写不进去（EAGAIN）时暂存待发数据
    PoolSendBuffer sendBuffer;
};

class EpollChatServer; // 前向声明

class SubReactor {
public:
    SubReactor(EpollChatServer* server);
    ~SubReactor();

    // 启动子 Reactor 的事件循环
    void run();
    
    // 向该子 Reactor 的 epoll 实例中添加一个新的文件描述符
    void addFd(const std::shared_ptr<ClientContext>& ctx);

private:
    int m_epollFd;
    EpollChatServer* m_server; // 指向主服务器对象，以便调用 handleRead/Write 等方法
    std::thread m_thread;
};




class EpollChatServer{
public:
    explicit EpollChatServer(uint16_t port);
    explicit EpollChatServer(uint16_t port, bool enableDBWrites);
    ~EpollChatServer();

    bool start();

private:
    uint16_t m_port ;
    int m_listenFd;
    int m_epollFd;
    std::unordered_map<int,  std::shared_ptr<ClientContext>> m_clients; // fd -> ClientContext
    std::unordered_map<std::string, std::weak_ptr<ClientContext>> m_onlineUsers; // accountID -> ClientContext
    //多线程
    std::mutex m_mapMutex;
    std::mutex m_sendMutex; // 保护发送操作的原子性

    std::vector<std::unique_ptr<SubReactor>> m_subReactors; // 存储所有从 Reactor
    std::atomic<size_t> m_nextSubReactor{0}; // 用于轮询选择下一个从 Reactor 的索引
    friend class SubReactor;

    ThreadPool m_threadPool;
    bool m_enableDBWrites = true;
    //内部辅助函数
   void log(const std::string& msg);
    void setNonBlocking(int fd);
    std::string extractJsonValue(const std::string& json, const std::string& key);
    // Epoll 事件驱动
    void run();
    void handleAccept();
    void handleRead(std::shared_ptr<ClientContext> ctx);
    void handleWrite(std::shared_ptr<ClientContext> ctx);
    void handleDisconnect(int fd);
    // 业务逻辑与发包机制
    void processPacket(std::shared_ptr<ClientContext> ctx, uint16_t msgType, const std::string& body);
    void sendPacket(const std::shared_ptr<ClientContext>& ctx, uint16_t type, const std::string& data);
    void sendPacket(int fd, uint16_t type, const std::string& data);
   //---------接入数据库-------------
    void saveMessageToDB(const std::string& sender, const std::string& target, const std::string& content);
    bool checkLoginFromDatabase(const std::string& inputUser, const std::string& inputPass);
    // 初始化数据库连接
    bool initDB();
    std::string getServerTimeStr(); 
    bool userExistsInDB(const std::string& username) ;
    bool addFriendToDB(const std::string& user, const std::string& friendName);
    std::vector<std::string> getFriendListFromDB(const std::string& username);

    bool initRedis();
    void setOnlineUser(const std::string& accountID, const std::shared_ptr<ClientContext>& ctx);
    std::shared_ptr<ClientContext> getOnlineCtx(const std::string& accountID);
    void removeOnlineUser(const std::string& accountID);

    std::string buildFriendListJson(const std::vector<std::string>& friends);
    std::string getFriendListJson(const std::string& username);
    void invalidateFriendListCache(const std::string& username);

    void pushOfflineMessage(const std::string& targetUser, const std::string& messageJson);
    std::vector<std::string> popOfflineMessages(const std::string& username);
};
#endif // EPOLL_CHAT_SERVER_H
