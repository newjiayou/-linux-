#ifndef EPOLL_CHAT_SERVER_H
#define EPOLL_CHAT_SERVER_H

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <algorithm>
#include "ThreadPool.h"
#include <memory>
#include <mutex>
#include <functional>
#include <deque>
#include <condition_variable>
#include <shared_mutex>
#include <mysql/mysql.h> // 新增：MySQL 头文件
#include <hiredis/hiredis.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <atomic> 
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <openssl/md5.h>
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

    void swap(PoolSendBuffer& other) noexcept {
        std::swap(m_capacity, other.m_capacity);
        std::swap(m_size, other.m_size);
        std::swap(m_offset, other.m_offset);
        std::swap(m_data, other.m_data);
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
        : m_buf(nextPowerOfTwo(initialCapacity)) {}

    PoolRecvBuffer(const PoolRecvBuffer&) = delete;
    PoolRecvBuffer& operator=(const PoolRecvBuffer&) = delete;

    void append(const void* src, size_t len) {
        if (len == 0) return;
        ensureCapacityForAppend(len);
        const uint8_t* in = static_cast<const uint8_t*>(src);
        const size_t mask = m_buf.size() - 1;
        size_t writePos = (m_read + m_size) & mask;
        size_t first = std::min(len, m_buf.size() - writePos);
        memcpy(m_buf.data() + writePos, in, first);
        if (len > first) {
            memcpy(m_buf.data(), in + first, len - first);
        }
        m_size += len;
    }

    size_t size() const { return m_size; }
    bool peek(void* dst, size_t len, size_t offset = 0) const {
        if (offset + len > m_size) return false;
        uint8_t* out = static_cast<uint8_t*>(dst);
        const size_t mask = m_buf.size() - 1;
        size_t start = (m_read + offset) & mask;
        size_t first = std::min(len, m_buf.size() - start);
        memcpy(out, m_buf.data() + start, first);
        if (len > first) {
            memcpy(out + first, m_buf.data(), len - first);
        }
        return true;
    }

    std::string peekString(size_t offset, size_t len) const {
        std::string out(len, '\0');
        if (len == 0) return out;
        if (!peek(out.data(), len, offset)) return {};
        return out;
    }

    void consume(size_t len) {
        if (len >= m_size) {
            m_read = 0;
            m_size = 0;
            return;
        }

        const size_t mask = m_buf.size() - 1;
        m_read = (m_read + len) & mask;
        m_size -= len;
    }

private:
    static size_t nextPowerOfTwo(size_t x) {
        size_t n = (x <= 1) ? 1 : x;
        if ((n & (n - 1)) == 0) return n;
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        if (sizeof(size_t) >= 8) {
            n |= n >> 32;
        }
        return n + 1;
    }

    void ensureCapacityForAppend(size_t appendLen) {
        if (m_buf.size() - m_size >= appendLen) return;

        size_t required = m_size + appendLen;
        size_t newCapacity = nextPowerOfTwo(required);

        std::vector<uint8_t> newBuf(newCapacity);
        if (m_size > 0) {
            size_t first = std::min(m_size, m_buf.size() - m_read);
            memcpy(newBuf.data(), m_buf.data() + m_read, first);
            if (m_size > first) {
                memcpy(newBuf.data() + first, m_buf.data(), m_size - first);
            }
        }
        m_buf.swap(newBuf);
        m_read = 0;
    }

private:
    std::vector<uint8_t> m_buf;
    size_t m_read = 0;
    size_t m_size = 0;
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
    // 双队列发送缓冲：
    // - pendingSendBuffer: 生产者线程写入
    // - activeSendBuffer : I/O 线程发送
    // I/O 线程在 active 为空时与 pending 交换，降低锁持有时间
    PoolSendBuffer pendingSendBuffer;
    PoolSendBuffer activeSendBuffer;
    std::atomic<bool> writeArmed{false}; // 是否已开启 EPOLLOUT 监听
    std::atomic<size_t> pendingBytes{0}; // 总待发送字节，用于背压判定
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
    ~EpollChatServer();

    bool start();

private:
    class SnowflakeIdGenerator {
    public:
        SnowflakeIdGenerator();
        uint64_t nextId();
    private:
        static constexpr uint64_t kEpochMs = 1704067200000ULL; // 2024-01-01 UTC
        static constexpr uint64_t kWorkerBits = 10;
        static constexpr uint64_t kSequenceBits = 12;
        static constexpr uint64_t kMaxWorkerId = (1ULL << kWorkerBits) - 1;
        static constexpr uint64_t kMaxSequence = (1ULL << kSequenceBits) - 1;
        static constexpr uint64_t kWorkerShift = kSequenceBits;
        static constexpr uint64_t kTimestampShift = kSequenceBits + kWorkerBits;

        uint64_t currentMs() const;
        uint64_t waitNextMs(uint64_t lastTs) const;

        std::mutex m_mutex;
        uint64_t m_workerId = 0;
        uint64_t m_lastTs = 0;
        uint64_t m_sequence = 0;
    };

    struct PendingChatLog {
        uint64_t msgId = 0;
        std::string sender;
        std::string target;
        std::string content;
    };

    static constexpr size_t kShardCount = 128;
    struct ClientShard {
        std::mutex mutex;
        std::unordered_map<int, std::shared_ptr<ClientContext>> clients;
    };
    struct OnlineUserShard {
        mutable std::shared_mutex mutex;
        std::unordered_map<std::string, std::weak_ptr<ClientContext>> users;
    };

    uint16_t m_port ;
    int m_listenFd;
    int m_epollFd;
    std::array<ClientShard, kShardCount> m_clientShards; // fd 分片
    std::array<OnlineUserShard, kShardCount> m_onlineUserShards; // accountID 分片
    std::mutex m_sendMutex; // 保护发送操作的原子性

    std::vector<std::unique_ptr<SubReactor>> m_subReactors; // 存储所有从 Reactor
    std::atomic<size_t> m_nextSubReactor{0}; // 用于轮询选择下一个从 Reactor 的索引
    friend class SubReactor;

    ThreadPool m_threadPool;
    std::unique_ptr<ThreadPool> m_dbThreadPool;
    std::unique_ptr<ThreadPool> m_redisThreadPool;
    bool m_enableDBWrites = true;
    std::atomic<size_t> m_dbTaskQueueDepth{0};
    std::atomic<size_t> m_redisTaskQueueDepth{0};
    static constexpr size_t kWorkerQueueLimit = 20000;
    static constexpr size_t kBizQueueLimit = 50000;
    static constexpr size_t kHistoryMaxRows = 500;
    std::mutex m_chatLogMutex;
    std::condition_variable m_chatLogCv;
    std::deque<PendingChatLog> m_chatLogActiveQueue;
    std::deque<PendingChatLog> m_chatLogFlushQueue;
    std::atomic<size_t> m_chatLogQueueDepth{0};
    std::thread m_chatLogFlushThread;
    std::atomic<bool> m_chatLogStop{false};
    static constexpr size_t kChatLogBatchSize = 128;
    static constexpr size_t kChatLogQueueLimit = 200000;
    static constexpr int kChatLogFlushIntervalMs = 20;
    std::thread m_retryThread;
    std::atomic<bool> m_retryStop{false};
    static constexpr int kRetryScanIntervalMs = 1000;
    static constexpr int kRetryDelayMs = 3000; // ACK 超时时间
    static constexpr size_t kRetryBatchPerUser = 50;
    static constexpr int kRetryMaxAttempts = 5;
    SnowflakeIdGenerator m_msgIdGen;

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
    void enqueueChatLog(const std::string& sender, const std::string& target, const std::string& content);
    void chatLogFlushLoop();
    void flushChatLogsBatch(const std::vector<PendingChatLog>& batch);
    std::string buildSessionId(const std::string& sender, const std::string& target) const;
    bool checkLoginFromDatabase(const std::string& inputUser, const std::string& inputPass);
    // 初始化数据库连接
    bool initDB();
    std::string getServerTimeStr(); 
    bool userExistsInDB(const std::string& username) ;
    bool addFriendToDB(const std::string& user, const std::string& friendName);
    std::vector<std::string> getFriendListFromDB(const std::string& username);

    bool initRedis();
    bool enqueueDbTask(std::function<void()> task);
    bool enqueueRedisTask(std::function<void()> task);
    bool isCurrentSessionValid(const std::shared_ptr<ClientContext>& ctx, int expectedFd) const;
    void setOnlineUser(const std::string& accountID, const std::shared_ptr<ClientContext>& ctx);
    std::shared_ptr<ClientContext> getOnlineCtx(const std::string& accountID);
    void removeOnlineUser(const std::string& accountID);
    std::shared_ptr<ClientContext> getClientCtxByFd(int fd);
    void addClientCtx(const std::shared_ptr<ClientContext>& ctx);
    void removeClientCtx(int fd);
    std::vector<std::shared_ptr<ClientContext>> snapshotAllClients();
    size_t clientShardIndex(int fd) const;
    size_t userShardIndex(const std::string& accountID) const;

    std::string buildFriendListJson(const std::vector<std::string>& friends);
    std::string getFriendListJson(const std::string& username);
    void invalidateFriendListCache(const std::string& username);

    uint64_t nextMsgId();
    std::string buildEnrichedChatBodyWithMsgId(
        uint64_t msgId,
        const std::string& sender,
        const std::string& target,
        const std::string& message,
        const std::string& ts);
    bool storePendingMessage(const std::string& targetUser, uint64_t msgId, const std::string& messageJson);
    void ackPendingMessage(const std::string& user, const std::vector<uint64_t>& msgIds);
    std::vector<std::string> fetchPendingMessages(const std::string& username, size_t limit, bool dueOnly);
    void dispatchDueMessagesForUser(const std::string& user, const std::shared_ptr<ClientContext>& ctx, bool includeAll);
    int getPendingAttempt(const std::string& user, const std::string& msgId);
    void setPendingAttempt(const std::string& user, const std::string& msgId, int attempt);
    std::vector<std::pair<std::string, std::shared_ptr<ClientContext>>> snapshotOnlineUsers();
    void retryLoop();
};
#endif // EPOLL_CHAT_SERVER_H
