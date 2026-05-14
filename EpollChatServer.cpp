#include "EpollChatServer.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <atomic>
#include <ctime>
#include <chrono>
#include <cctype>
#include <openssl/md5.h>
#include "MemoryPool.h"

#define MAX_EVENTS 1024
#define READ_BUFFER_SIZE 4096
#define MAX_PACKET_SIZE (1024 * 1024)
#define MAX_PENDING_SEND_BUFFER (16 * 1024 * 1024)
// 单次 EPOLLOUT 事件最多刷出的字节，避免单连接长期占用 reactor 线程
#define WRITE_FLUSH_BUDGET_BYTES (64 * 1024)

// 新增：服务器消息接收确认 ACK 包类型
static constexpr uint16_t MSG_TYPE_ACK = 13;

// ---- 统计用原子计数器：帮助定位卡在哪个阶段 ----
static std::atomic<long> g_acceptCount{0};     // 成功 accept 的连接数
static std::atomic<long> g_readCount{0};       // 成功 recv（bytesRead > 0）的次数
static std::atomic<long> g_loginIn{0};         // 进入登录逻辑（msgType == 4）的次数
static std::atomic<long> g_loginDB{0};         // 执行到 DB 查询阶段的次数-阿迪王
static std::atomic<long> g_loginOk{0};         // 登录成功发送前的次数
static std::atomic<long> g_loginFail{0};       // 登录失败发送前的次数
static std::atomic<long> g_sendCalled{0};      // sendPacket 被调用的总次数
static std::atomic<long> g_backpressureDrop{0}; // 因发送积压过大而断开的次数
static std::atomic<long> g_dbTaskEnqueue{0};
static std::atomic<long> g_dbTaskDone{0};
static std::atomic<long> g_dbTaskTimeout{0};
static std::atomic<long> g_redisTaskEnqueue{0};
static std::atomic<long> g_redisTaskDone{0};
static std::atomic<long> g_workerQueueDepth{0};
static std::atomic<long> g_chatLogDrop{0};
static std::atomic<long long> g_privateGetOnlineNs{0};
static std::atomic<long long> g_privateSendNs{0};
static std::atomic<long long> g_privateMsgCount{0};

// V2 内存池封装：用于在服务器里安全地申请/释放临时内存
class PoolBuffer {
public:
    explicit PoolBuffer(size_t size)
        : m_size(size), m_ptr(Kama_memoryPool::MemoryPool::allocate(size)) {}

    ~PoolBuffer() {
        if (m_ptr) {
            Kama_memoryPool::MemoryPool::deallocate(m_ptr, m_size);
        }
    }

    PoolBuffer(const PoolBuffer&) = delete;
    PoolBuffer& operator=(const PoolBuffer&) = delete;

    void* data() { return m_ptr; }

private:
    size_t m_size;
    void* m_ptr;
};

namespace {

bool extractJsonStringFast(const std::string& json, const char* key, std::string& out);
std::vector<uint64_t> parseJsonUint64ArrayFast(const std::string& json, const char* key, size_t maxCount);

static std::string extractJsonValueLite(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    pos = json.find(":", pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '"')) pos++;
    size_t endPos = pos;
    while (endPos < json.length() && json[endPos] != '"' && json[endPos] != ',' && json[endPos] != '}') endPos++;
    return json.substr(pos, endPos - pos);
}

namespace pb {
static bool writeVarint(uint64_t v, std::string& out) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
    return true;
}

static bool readVarint(const std::string& in, size_t& off, uint64_t& v) {
    v = 0;
    int shift = 0;
    while (off < in.size() && shift <= 63) {
        uint8_t b = static_cast<uint8_t>(in[off++]);
        v |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

static void writeKey(uint32_t fieldNo, uint8_t wireType, std::string& out) {
    writeVarint((static_cast<uint64_t>(fieldNo) << 3) | wireType, out);
}

static void writeString(uint32_t fieldNo, const std::string& s, std::string& out) {
    writeKey(fieldNo, 2, out);
    writeVarint(s.size(), out);
    out.append(s);
}

static void writeUint64(uint32_t fieldNo, uint64_t v, std::string& out) {
    writeKey(fieldNo, 0, out);
    writeVarint(v, out);
}

static bool skipField(uint8_t wt, const std::string& in, size_t& off) {
    if (wt == 0) {
        uint64_t tmp = 0;
        return readVarint(in, off, tmp);
    }
    if (wt == 2) {
        uint64_t len = 0;
        if (!readVarint(in, off, len)) return false;
        if (off + len > in.size()) return false;
        off += static_cast<size_t>(len);
        return true;
    }
    return false;
}
}

static uint64_t parseUint64Safe(const std::string& s) {
    if (s.empty()) return 0;
    uint64_t v = 0;
    for (char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return 0;
        v = v * 10 + static_cast<uint64_t>(ch - '0');
    }
    return v;
}

static std::string decodeProtoToJson(uint16_t msgType, const std::string& body) {
    if (body.empty()) return "";
    std::string sender, target, message, timestamp, result, username, password, user, lastTs, peer, content, friendName, errMsg;
    uint64_t msgId = 0;
    uint64_t ackType = 0;
    std::vector<uint64_t> msgIds;
    size_t off = 0;
    while (off < body.size()) {
        uint64_t key = 0;
        if (!pb::readVarint(body, off, key)) return "";
        const uint32_t field = static_cast<uint32_t>(key >> 3);
        const uint8_t wt = static_cast<uint8_t>(key & 0x7);
        if (wt == 2) {
            uint64_t len = 0;
            if (!pb::readVarint(body, off, len)) return "";
            if (off + len > body.size()) return "";
            std::string v = body.substr(off, static_cast<size_t>(len));
            off += static_cast<size_t>(len);
            if (field == 1) {
                if (msgType == 4) username = v;
                else if (msgType == 5 || msgType == 10) result = v;
                else if (msgType == 7) lastTs = v;
                else if (msgType == 9) friendName = v;
                else if (msgType == 13) result = v;
                else sender = v;
            } else if (field == 2) {
                if (msgType == 4) password = v;
                else if (msgType == 5 || msgType == 10 || msgType == 13) errMsg = v;
                else peer = v;
            } else if (field == 3) {
                if (msgType == 13) {
                    uint64_t tmp = parseUint64Safe(v);
                    if (tmp > 0) msgId = tmp;
                } else message = v;
            } else if (field == 4) {
                if (msgType == 13) user = v;
                else {
                    uint64_t tmp = parseUint64Safe(v);
                    if (tmp > 0) msgId = tmp;
                }
            } else if (field == 5) {
                if (msgType == 13) errMsg = v;
                else timestamp = v;
            }
        } else if (wt == 0) {
            uint64_t v = 0;
            if (!pb::readVarint(body, off, v)) return "";
            if (field == 2 && msgType == 13) ackType = v;
            else if (field == 3 && msgType == 13) msgId = v;
            else if (field == 4) msgId = v;
            else if (field == 5 && msgType == 13) msgIds.push_back(v);
        } else {
            if (!pb::skipField(wt, body, off)) return "";
        }
    }

    if (msgType == 1 || msgType == 3 || msgType == 7 || msgType == 9 || msgType == 11 || msgType == 4 || msgType == 13) {
        std::string out = "{";
        bool first = true;
        auto addKV = [&](const std::string& k, const std::string& v) {
            if (v.empty()) return;
            if (!first) out += ",";
            out += "\"" + k + "\":\"" + v + "\"";
            first = false;
        };
        auto addKVNum = [&](const std::string& k, uint64_t v) {
            if (v == 0) return;
            if (!first) out += ",";
            out += "\"" + k + "\":\"" + std::to_string(v) + "\"";
            first = false;
        };
        if (msgType == 1) {
            addKV("sender", sender); addKV("target", target); addKV("message", message); addKV("timestamp", timestamp); addKVNum("msg_id", msgId);
        } else if (msgType == 3) {
            addKV("sender", sender);
        } else if (msgType == 4) {
            addKV("username", username); addKV("password", password);
        } else if (msgType == 7) {
            addKV("last_timestamp", lastTs); addKV("peer", peer);
        } else if (msgType == 9) {
            addKV("friend", friendName);
        } else if (msgType == 11) {
        } else if (msgType == 13) {
            addKV("result", result); if (ackType > 0) addKVNum("type", ackType); addKVNum("msg_id", msgId); addKV("user", user); addKV("message", errMsg);
            if (!msgIds.empty()) {
                if (!first) out += ",";
                out += "\"msg_ids\":[";
                for (size_t i = 0; i < msgIds.size(); ++i) {
                    if (i) out += ",";
                    out += std::to_string(msgIds[i]);
                }
                out += "]";
            }
        }
        out += "}";
        return out;
    }
    return "";
}

static std::string encodeJsonToProto(uint16_t msgType, const std::string& json) {
    if (json.empty()) return "";
    std::string out;
    out.reserve(json.size());
    auto get = [&](const char* k) { std::string v; extractJsonStringFast(json, k, v); if (v.empty()) v = extractJsonValueLite(json, k); return v; };

    if (msgType == 1) {
        std::string sender = get("sender"), target = get("target"), message = get("message"), ts = get("timestamp"), msgId = get("msg_id");
        if (!sender.empty()) pb::writeString(1, sender, out);
        if (!target.empty()) pb::writeString(2, target, out);
        if (!message.empty()) pb::writeString(3, message, out);
        uint64_t id = parseUint64Safe(msgId);
        if (id > 0) pb::writeUint64(4, id, out);
        if (!ts.empty()) pb::writeString(5, ts, out);
    } else if (msgType == 3) {
        std::string sender = get("sender");
        if (!sender.empty()) pb::writeString(1, sender, out);
    } else if (msgType == 4) {
        std::string username = get("username"), password = get("password");
        if (!username.empty()) pb::writeString(1, username, out);
        if (!password.empty()) pb::writeString(2, password, out);
    } else if (msgType == 5) {
        std::string result = get("result"), message = get("message");
        if (!result.empty()) pb::writeString(1, result, out);
        if (!message.empty()) pb::writeString(2, message, out);
    } else if (msgType == 7) {
        std::string lastTs = get("last_timestamp");
        std::string peer = get("peer");
        if (peer.empty()) peer = get("target");
        if (!lastTs.empty()) pb::writeString(1, lastTs, out);
        if (!peer.empty()) pb::writeString(2, peer, out);
    } else if (msgType == 9) {
        std::string f = get("friend");
        if (!f.empty()) pb::writeString(1, f, out);
    } else if (msgType == 10) {
        std::string result = get("result"), message = get("message"), friendName = get("friend");
        if (!result.empty()) pb::writeString(1, result, out);
        if (!message.empty()) pb::writeString(2, message, out);
        if (!friendName.empty()) pb::writeString(3, friendName, out);
    } else if (msgType == 12) {
        // 保持兼容：好友列表结构暂不改造，直接透传 JSON。
        return json;
    } else if (msgType == 13) {
        std::string result = get("result"), type = get("type"), msgId = get("msg_id"), user = get("user"), message = get("message");
        if (!result.empty()) pb::writeString(1, result, out);
        uint64_t t = parseUint64Safe(type); if (t > 0) pb::writeUint64(2, t, out);
        uint64_t id = parseUint64Safe(msgId); if (id > 0) pb::writeUint64(3, id, out);
        if (!user.empty()) pb::writeString(4, user, out);
        if (!message.empty()) pb::writeString(5, message, out);
        auto ids = parseJsonUint64ArrayFast(json, "msg_ids", 256);
        for (auto v : ids) if (v > 0) pb::writeUint64(5, v, out);
    } else {
        return json;
    }

    return out;
}

bool extractJsonStringFast(const std::string& json, const char* key, std::string& out) {
    // 轻量解析："key" : "value"
    const size_t n = json.size();
    const size_t klen = std::strlen(key);
    if (klen == 0 || n < klen + 2) return false;

    size_t i = 0;
    while (i + klen + 2 <= n) {
        if (json[i] != '"') {
            ++i;
            continue;
        }
        if (i + 1 + klen >= n) return false;
        if (std::memcmp(json.data() + i + 1, key, klen) != 0 || json[i + 1 + klen] != '"') {
            ++i;
            continue;
        }
        
        size_t p = i + 1 + klen + 1;
        while (p < n && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
        if (p >= n || json[p] != ':') return false;
        ++p;
        while (p < n && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) ++p;
        if (p >= n || json[p] != '"') return false;
        ++p;

        size_t start = p;
        while (p < n) {
            if (json[p] == '"' && (p == start || json[p - 1] != '\\')) break;
            ++p;
        }
        if (p >= n) return false;
        out.assign(json.data() + start, p - start);
        return true;
    }
    return false;
}

bool parseChatFieldsFast(const std::string& json, std::string& sender, std::string& target, std::string& message) {
    return extractJsonStringFast(json, "sender", sender) &&
           extractJsonStringFast(json, "target", target) &&
           extractJsonStringFast(json, "message", message);
}

std::vector<uint64_t> parseJsonUint64ArrayFast(const std::string& json, const char* key, size_t maxCount = 256) {
    std::vector<uint64_t> out;
    if (!key || *key == '\0') return out;
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return out;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return out;
    ++pos;
    const size_t n = json.size();
    while (pos < n && out.size() < maxCount) {
        while (pos < n && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n' || json[pos] == ',')) ++pos;
        if (pos >= n || json[pos] == ']') break;
        size_t start = pos;
        while (pos < n && json[pos] >= '0' && json[pos] <= '9') ++pos;
        if (start == pos) {
            while (pos < n && json[pos] != ',' && json[pos] != ']') ++pos;
            if (pos < n && json[pos] == ']') break;
            continue;
        }
        try {
            out.push_back(std::stoull(json.substr(start, pos - start)));
        } catch (...) {
        }
        while (pos < n && json[pos] != ',' && json[pos] != ']') ++pos;
        if (pos < n && json[pos] == ']') break;
    }
    return out;
}

std::string currentTimeStrCached() {
    thread_local std::time_t tl_last_sec = 0;
    thread_local char tl_buf[20] = {0}; // "YYYY-MM-DD HH:MM:SS"

    const std::time_t now = std::time(nullptr);
    if (now != tl_last_sec) {
        std::tm tmv{};
        localtime_r(&now, &tmv);
        std::strftime(tl_buf, sizeof(tl_buf), "%Y-%m-%d %H:%M:%S", &tmv);
        tl_last_sec = now;
    }
    return std::string(tl_buf);
}

std::string buildEnrichedChatBody(
    const std::string& sender,
    const std::string& target,
    const std::string& message,
    const std::string& ts
) {
    std::string out;
    out.reserve(sender.size() + target.size() + message.size() + ts.size() + 64);
    out += "{\"sender\":\"";
    out += sender;
    out += "\",\"target\":\"";
    out += target;
    out += "\",\"message\":\"";
    out += message;
    out += "\",\"timestamp\":\"";
    out += ts;
    out += "\"}";
    return out;
}
} // namespace

EpollChatServer::SnowflakeIdGenerator::SnowflakeIdGenerator() {
    m_workerId = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())) & kMaxWorkerId;
}

uint64_t EpollChatServer::SnowflakeIdGenerator::currentMs() const {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

uint64_t EpollChatServer::SnowflakeIdGenerator::waitNextMs(uint64_t lastTs) const {
    uint64_t ts = currentMs();
    while (ts <= lastTs) ts = currentMs();
    return ts;
}

uint64_t EpollChatServer::SnowflakeIdGenerator::nextId() {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t ts = currentMs();
    if (ts < m_lastTs) {
        ts = m_lastTs;
    }
    if (ts == m_lastTs) {
        m_sequence = (m_sequence + 1) & kMaxSequence;
        if (m_sequence == 0) {
            ts = waitNextMs(m_lastTs);
        }
    } else {
        m_sequence = 0;
    }
    m_lastTs = ts;
    const uint64_t delta = (ts > kEpochMs) ? (ts - kEpochMs) : 0;
    return (delta << kTimestampShift) | (m_workerId << kWorkerShift) | m_sequence;
}

SubReactor::SubReactor(EpollChatServer* server) : m_server(server) {
    m_epollFd = epoll_create1(0);
    if (m_epollFd < 0) {
        throw std::runtime_error("Failed to create sub-reactor epoll instance");
    }
    // 创建时即启动线程
    m_thread = std::thread(&SubReactor::run, this);
}

SubReactor::~SubReactor() {
    if (m_thread.joinable()) {
        // 通常需要一个机制来优雅地停止线程，这里为了简化，直接 detach
        // 在生产环境中，应该发送一个信号让 run() 循环退出
        m_thread.detach(); 
    }
    if (m_epollFd != -1) {
        close(m_epollFd);
    }
}

void SubReactor::addFd(const std::shared_ptr<ClientContext>& ctx) {
    ctx->reactorEpollFd = m_epollFd;

    struct epoll_event event{};
    event.data.fd = ctx->fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(m_epollFd, EPOLL_CTL_ADD, ctx->fd, &event);
}


void SubReactor::run() {
    struct epoll_event events[MAX_EVENTS];
    while (true) {
        int numEvents = epoll_wait(m_epollFd, events, MAX_EVENTS, -1);
        for (int i = 0; i < numEvents; i++) {
            int fd = events[i].data.fd;
            if (fd <= 0) continue;

            std::shared_ptr<ClientContext> ctx;
            ctx = m_server->getClientCtxByFd(fd);
            if (!ctx) continue;

            uint32_t ev = events[i].events;

            if (ev & EPOLLIN) m_server->handleRead(ctx);
            if (ev & EPOLLOUT) m_server->handleWrite(ctx);
            if (ev & (EPOLLERR | EPOLLHUP)) m_server->handleDisconnect(fd);
        }
    }
}
// 构造函数
EpollChatServer::EpollChatServer(uint16_t port) 
    : m_port(port),
      m_listenFd(-1),
      m_epollFd(-1),
      m_threadPool(12, kBizQueueLimit),
      m_dbThreadPool(std::make_unique<ThreadPool>(20)),
      m_redisThreadPool(std::make_unique<ThreadPool>(16)) {}

EpollChatServer::~EpollChatServer() {
    m_retryStop.store(true);
    if (m_retryThread.joinable()) {
        m_retryThread.join();
    }
    m_chatLogStop.store(true);
    m_chatLogCv.notify_all();
    if (m_chatLogFlushThread.joinable()) {
        m_chatLogFlushThread.join();
    }
    m_subReactors.clear(); 
    if (m_listenFd != -1) close(m_listenFd);
    if (m_epollFd != -1) close(m_epollFd);
    auto allClients = snapshotAllClients();
    for (const auto& ctx : allClients) {
        if (ctx) close(ctx->fd);
    }
}

size_t EpollChatServer::clientShardIndex(int fd) const {
    return static_cast<size_t>(fd) % kShardCount;
}

size_t EpollChatServer::userShardIndex(const std::string& accountID) const {
    return std::hash<std::string>{}(accountID) % kShardCount;
}

void EpollChatServer::addClientCtx(const std::shared_ptr<ClientContext>& ctx) {
    if (!ctx) return;
    auto& shard = m_clientShards[clientShardIndex(ctx->fd)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.clients[ctx->fd] = ctx;
}

void EpollChatServer::removeClientCtx(int fd) {
    auto& shard = m_clientShards[clientShardIndex(fd)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    shard.clients.erase(fd);
}

std::shared_ptr<ClientContext> EpollChatServer::getClientCtxByFd(int fd) {
    auto& shard = m_clientShards[clientShardIndex(fd)];
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.clients.find(fd);
    if (it == shard.clients.end()) return nullptr;
    return it->second;
}

std::vector<std::shared_ptr<ClientContext>> EpollChatServer::snapshotAllClients() {
    std::vector<std::shared_ptr<ClientContext>> targets;
    for (auto& shard : m_clientShards) {
        std::lock_guard<std::mutex> lock(shard.mutex);
        targets.reserve(targets.size() + shard.clients.size());
        for (const auto& pair : shard.clients) targets.push_back(pair.second);
    }
    return targets;
}

// 保留函数定义
void EpollChatServer::log(const std::string& msg) {
    std::cout << "[LOG] " << msg << std::endl;
}

void EpollChatServer::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool EpollChatServer::enqueueDbTask(std::function<void()> task) {
    const size_t depth = m_dbTaskQueueDepth.fetch_add(1) + 1;
    g_workerQueueDepth.store((long)depth);
    if (depth > kWorkerQueueLimit) {
        m_dbTaskQueueDepth.fetch_sub(1);
        g_dbTaskTimeout.fetch_add(1);
        return false;
    }
    g_dbTaskEnqueue.fetch_add(1);
    m_dbThreadPool->enqueue([this, task = std::move(task)]() mutable {
        task();
        g_dbTaskDone.fetch_add(1);
        const size_t newDepth = m_dbTaskQueueDepth.fetch_sub(1) - 1;
        g_workerQueueDepth.store((long)newDepth);
    });
    return true;
}

bool EpollChatServer::enqueueRedisTask(std::function<void()> task) {
    const size_t depth = m_redisTaskQueueDepth.fetch_add(1) + 1;
    g_workerQueueDepth.store((long)std::max(depth, m_dbTaskQueueDepth.load()));
    if (depth > kWorkerQueueLimit) {
        m_redisTaskQueueDepth.fetch_sub(1);
        g_dbTaskTimeout.fetch_add(1);
        return false;
    }
    g_redisTaskEnqueue.fetch_add(1);
    m_redisThreadPool->enqueue([this, task = std::move(task)]() mutable {
        task();
        g_redisTaskDone.fetch_add(1);
        const size_t newDepth = m_redisTaskQueueDepth.fetch_sub(1) - 1;
        g_workerQueueDepth.store((long)std::max(newDepth, m_dbTaskQueueDepth.load()));
    });
    return true;
}

bool EpollChatServer::isCurrentSessionValid(const std::shared_ptr<ClientContext>& ctx, int expectedFd) const {
    if (!ctx) return false;
    return ctx->fd == expectedFd;
}

std::string EpollChatServer::extractJsonValue(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    pos = json.find(":", pos);
    if (pos == std::string::npos) return "";
    pos++; 
    
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\"')) pos++;
    size_t endPos = pos;
    while (endPos < json.length() && json[endPos] != '\"' && json[endPos] != ',' && json[endPos] != '}') endPos++;
    return json.substr(pos, endPos - pos);
}

bool EpollChatServer::start() {
    m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_listenFd < 0) return false;

    int opt = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setNonBlocking(m_listenFd);

    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(m_port);

    if (bind(m_listenFd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) return false;
    if (listen(m_listenFd, SOMAXCONN) < 0) return false;
     if (!initDB()) return false;
     if (!initRedis()) return false;
    m_chatLogStop.store(false);
    m_chatLogFlushThread = std::thread(&EpollChatServer::chatLogFlushLoop, this);
    m_retryStop.store(false);
    m_retryThread = std::thread(&EpollChatServer::retryLoop, this);
    m_epollFd = epoll_create1(0);
    if (m_epollFd < 0) return false;

    struct epoll_event event{};
    event.data.fd = m_listenFd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_listenFd, &event);
    //创建子epoll
    unsigned int numSubReactors = std::thread::hardware_concurrency(); // 获取CPU核心数作为子Reactor数量
    if (numSubReactors == 0) numSubReactors = 4; // 备用值
    for (unsigned int i = 0; i < numSubReactors; ++i) {
        m_subReactors.emplace_back(std::make_unique<SubReactor>(this));
    }
    log("启动 " + std::to_string(numSubReactors) + " 个 I/O 线程 (Sub-Reactors)");



    // log("服务器启动成功，监听端口: " + std::to_string(m_port));
    run();
    return true;
}

void EpollChatServer::run() {
    struct epoll_event events[MAX_EVENTS];
    auto lastPrint = std::chrono::steady_clock::now();
    while (true) {
        int numEvents = epoll_wait(m_epollFd, events, MAX_EVENTS, -1);
        for (int i = 0; i < numEvents; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;
            if (fd == m_listenFd) {
                if (ev & EPOLLIN) handleAccept();
            }

        }

        // 每隔几秒打印一次统计信息，方便压测后观察
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPrint).count() >= 5) {
            lastPrint = now;
            std::cout << "[STATS] accept=" << g_acceptCount.load()
                      << " read=" << g_readCount.load()
                      << " login_in=" << g_loginIn.load()
                      << " login_db=" << g_loginDB.load()
                      << " login_ok=" << g_loginOk.load()
                      << " login_fail=" << g_loginFail.load()
                      << " send_calls=" << g_sendCalled.load()
                      << " backpressure_drop=" << g_backpressureDrop.load()
                      << " db_task_enqueue=" << g_dbTaskEnqueue.load()
                      << " db_task_done=" << g_dbTaskDone.load()
                      << " db_task_timeout=" << g_dbTaskTimeout.load()
                      << " redis_task_enqueue=" << g_redisTaskEnqueue.load()
                      << " redis_task_done=" << g_redisTaskDone.load()
                      << " chatlog_q=" << m_chatLogQueueDepth.load()
                      << " chatlog_drop=" << g_chatLogDrop.load()
                      << " worker_queue_depth=" << g_workerQueueDepth.load();
            const long long pmsg = g_privateMsgCount.load();
            if (pmsg > 0) {
                const long long avgGetNs = g_privateGetOnlineNs.load() / pmsg;
                const long long avgSendNs = g_privateSendNs.load() / pmsg;
                std::cout << " private_msg=" << pmsg
                          << " avg_getonline_ns=" << avgGetNs
                          << " avg_send_ns=" << avgSendNs;
            }
            std::cout
                      << std::endl;
        }
    }
}

void EpollChatServer::handleAccept() {
    while (true) {
        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept4(m_listenFd, (struct sockaddr*)&clientAddr, &clientLen, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 已经把当前可 accept 的连接全部取完
                break;
            }
            // 其他错误：本轮结束，等待下次 EPOLLIN
            break;
        }

        // accept4 已设置 NONBLOCK，这里不再重复设置

        // 降低小包回传延迟：关闭 Nagle，避免聚包等待
        int one = 1;
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        auto ctx = std::make_shared<ClientContext>();
        ctx->fd = clientFd;
        ctx->ip = inet_ntoa(clientAddr.sin_addr);

        addClientCtx(ctx);

        if (!m_subReactors.empty()) {
            size_t index = m_nextSubReactor.fetch_add(1) % m_subReactors.size();
            m_subReactors[index]->addFd(ctx);
        } else {
            // 如果没有子 Reactor（不应该发生），作为备用直接关闭
            removeClientCtx(clientFd);
            close(clientFd);
            continue;
        }

        ++g_acceptCount;

        // log("新物理连接: " + ctx->ip + " (fd: " + std::to_string(clientFd) + ")");
    }
}

void EpollChatServer::handleRead(std::shared_ptr<ClientContext> ctx) {
    if (!ctx) return;

    int fd = ctx->fd;
    bool shouldDisconnect = false;
    char buf[READ_BUFFER_SIZE];
    std::vector<std::pair<uint16_t, std::string>> pendingPackets;

    // 循环读到 EAGAIN，尽量一次性清空内核接收缓冲
    while (true) {
        int bytesRead = recv(fd, buf, sizeof(buf), 0);

        if (bytesRead > 0) {
            ++g_readCount;
            std::lock_guard<std::mutex> lock(ctx->clientMutex);
            ctx->buffer.append(buf, (size_t)bytesRead);
            continue;
        }

        if (bytesRead == 0) {
            shouldDisconnect = true;
            break;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        shouldDisconnect = true;
        break;
    }

    if (shouldDisconnect) {
        handleDisconnect(fd);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(ctx->clientMutex);

        // 循环拆包处理粘包
        while (ctx->buffer.size() >= sizeof(uint32_t)) {
            uint32_t totalLength;
            if (!ctx->buffer.peek(&totalLength, sizeof(uint32_t), 0)) {
                break;
            }
            totalLength = ntohl(totalLength);

            // 非法包长保护：至少要包含 length(4)+type(2)，且不能无限大
            if (totalLength < 6 || totalLength > MAX_PACKET_SIZE) {
                shouldDisconnect = true;
                break;
            }

            if (ctx->buffer.size() < totalLength) {
                // 数据还没收全，继续等待
                break;
            }

            uint16_t msgType;
            if (!ctx->buffer.peek(&msgType, sizeof(uint16_t), 4)) {
                break;
            }
            msgType = ntohs(msgType);

            std::string body = ctx->buffer.peekString(6, totalLength - 6);
            body = decodeProtoToJson(msgType, body);

            // 移除已处理数据
            ctx->buffer.consume(totalLength);
            pendingPackets.emplace_back(msgType, std::move(body));
        }
    }

    if (shouldDisconnect) {
        handleDisconnect(fd);
        return;
    }

    for (auto& packet : pendingPackets) {
        uint16_t msgType = packet.first;
        std::string body = std::move(packet.second);
        if (msgType == 2) {
            // 心跳包直接在 I/O 线程快速响应，避免排队抖动
            sendPacket(ctx, 2, "");
            continue;
        }
        if (!m_threadPool.try_enqueue([this, ctx, msgType, body = std::move(body)]() mutable {
            this->processPacket(ctx, msgType, body);
        })) {
            // 主业务队列已满，快速失败，避免整体排队雪崩
            sendPacket(ctx, MSG_TYPE_ACK, "{\"result\":\"fail\",\"message\":\"server_busy\"}");
        }
    }
}

void EpollChatServer::processPacket(std::shared_ptr<ClientContext> ctx, uint16_t msgType, const std::string& body) {
    if (msgType == 3) {
        std::string senderID = extractJsonValue(body, "sender");
        if (!senderID.empty()) {
            setOnlineUser(senderID, ctx);
            ctx->accountID = senderID;
            // log("身份识别: " + senderID + " 已绑定 fd: " + std::to_string(clientFd));
        }
    }
    else if (msgType == 1) {
        std::string senderID;
        std::string target;
        std::string content;
        if (!parseChatFieldsFast(body, senderID, target, content)) {
            senderID = extractJsonValue(body, "sender");
            target = extractJsonValue(body, "target");
            content = extractJsonValue(body, "message");
        }

        const uint64_t msgId = nextMsgId();
        std::string enrichedBody = buildEnrichedChatBodyWithMsgId(msgId, senderID, target, content, currentTimeStrCached());
        std::string ackBody = "{\"result\":\"received\",\"type\":1,\"msg_id\":\"" + std::to_string(msgId) + "\"}";

        if (target == "broadcast") {
            // log("执行广播消息，来源: " + senderID);
            std::vector<std::shared_ptr<ClientContext>> targets = snapshotAllClients();
            for (const auto& tCtx : targets) sendPacket(tCtx, 1, enrichedBody);
            sendPacket(ctx, MSG_TYPE_ACK, ackBody);
        } else {
            auto tLookup0 = std::chrono::steady_clock::now();
            std::shared_ptr<ClientContext> targetCtx = getOnlineCtx(target);
            auto tLookup1 = std::chrono::steady_clock::now();
            g_privateGetOnlineNs.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(tLookup1 - tLookup0).count()
            );

            auto tSend0 = std::chrono::steady_clock::now();
            int expectedSenderFd = ctx ? ctx->fd : -1;
            bool queued = enqueueRedisTask([this, ctx, expectedSenderFd, target, targetCtx, enrichedBody, ackBody, msgId]() {
                if (!storePendingMessage(target, msgId, enrichedBody)) {
                    return;
                }
                if (isCurrentSessionValid(ctx, expectedSenderFd)) {
                    sendPacket(ctx, MSG_TYPE_ACK, ackBody);
                }
                if (targetCtx && isCurrentSessionValid(targetCtx, targetCtx->fd)) {
                    sendPacket(targetCtx, 1, enrichedBody);
                }
            });
            if (!queued) {
                // Redis 任务未入队时不回发成功 ACK
            }
            auto tSend1 = std::chrono::steady_clock::now();
            g_privateSendNs.fetch_add(
                std::chrono::duration_cast<std::chrono::nanoseconds>(tSend1 - tSend0).count()
            );
            g_privateMsgCount.fetch_add(1);
        }

        // 聊天消息先入内存队列，后台批量落库，降低每条消息 enqueue+DB 往返开销
        enqueueChatLog(senderID, target, content);
    }
    else if (msgType == 2) {
        // 心跳回应
        sendPacket(ctx, 2, "");
    }
    else if (msgType == 4) {
        ++g_loginIn;
        std::string username = extractJsonValue(body, "username");
        std::string password = extractJsonValue(body, "password");
        int expectedFd = ctx ? ctx->fd : -1;
        auto start = std::chrono::steady_clock::now();

        bool queued = enqueueDbTask([this, ctx, expectedFd, username, password, start]() {
            if (!isCurrentSessionValid(ctx, expectedFd)) return;
            if (!checkLoginFromDatabase(username, password)) {
                ++g_loginFail;
                if (isCurrentSessionValid(ctx, expectedFd)) {
                    sendPacket(ctx, 5, "{\"result\":\"fail\"}");
                }
                return;
            }

            ++g_loginOk;
            if (!isCurrentSessionValid(ctx, expectedFd)) return;
            setOnlineUser(username, ctx);
            ctx->accountID = username;
            sendPacket(ctx, 5, "{\"result\":\"success\"}");

            if (isCurrentSessionValid(ctx, expectedFd)) {
                sendPacket(ctx, 12, getFriendListJson(username));
            }

            if (isCurrentSessionValid(ctx, expectedFd)) {
                dispatchDueMessagesForUser(username, ctx, true);
            }

            (void)start;
        });
        if (!queued) {
            ++g_loginFail;
            sendPacket(ctx, 5, "{\"result\":\"fail\",\"message\":\"system_busy\"}");
        }
    }
    else if (msgType == MSG_TYPE_ACK) {
        std::string user = extractJsonValue(body, "user");
        if (user.empty()) user = ctx ? ctx->accountID : "";
        if (user.empty()) return;

        std::vector<uint64_t> msgIds = parseJsonUint64ArrayFast(body, "msg_ids");
        if (msgIds.empty()) {
            std::string msgIdStr = extractJsonValue(body, "msg_id");
            if (!msgIdStr.empty()) {
                try {
                    uint64_t one = std::stoull(msgIdStr);
                    if (one > 0) msgIds.push_back(one);
                } catch (...) {
                }
            }
        }
        if (msgIds.empty()) return;

        enqueueRedisTask([this, user, msgIds = std::move(msgIds)]() mutable {
            this->ackPendingMessage(user, msgIds);
        });
    }
    else if (msgType == 7) {
        std::string lastTime = extractJsonValue(body, "last_timestamp");
        std::string peer = extractJsonValue(body, "peer");
        if (peer.empty()) peer = extractJsonValue(body, "target");
        std::string currentUser = ctx->accountID; 
        if (currentUser.empty() || peer.empty()) {
            sendPacket(ctx, 8, "[]");
            return;
        }
        int expectedFd = ctx ? ctx->fd : -1;
        const std::string sessionId = buildSessionId(currentUser, peer);

        if (!enqueueDbTask([this, ctx, expectedFd, lastTime, currentUser, sessionId]() {
                auto conn_ptr = DBConnectionPool::getInstance().getConnection();
                MYSQL* m_mysql = conn_ptr.get();
                MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
                const char* sql = "SELECT sender, target, content, created_at FROM all_messages_log "
                                  "WHERE session_id = ? AND created_at > ? "
                                  "ORDER BY created_at ASC LIMIT ?";

                if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
                    mysql_stmt_close(stmt);
                    return;
                }

                int limitRows = static_cast<int>(kHistoryMaxRows);
                MYSQL_BIND bind_in[3];
                memset(bind_in, 0, sizeof(bind_in));
                bind_in[0].buffer_type = MYSQL_TYPE_STRING;
                bind_in[0].buffer = (char*)sessionId.c_str();
                bind_in[0].buffer_length = sessionId.length();
                bind_in[1].buffer_type = MYSQL_TYPE_STRING;
                bind_in[1].buffer = (char*)lastTime.c_str();
                bind_in[1].buffer_length = lastTime.length();
                bind_in[2].buffer_type = MYSQL_TYPE_LONG;
                bind_in[2].buffer = &limitRows;

                mysql_stmt_bind_param(stmt, bind_in);
                mysql_stmt_execute(stmt);

                char s_buf[64], t_buf[64], c_buf[1024], ts_buf[64];
                unsigned long s_len, t_len, c_len, ts_len;
                MYSQL_BIND bind_out[4];
                memset(bind_out, 0, sizeof(bind_out));
                bind_out[0].buffer_type = MYSQL_TYPE_STRING; bind_out[0].buffer = s_buf; bind_out[0].buffer_length = sizeof(s_buf); bind_out[0].length = &s_len;
                bind_out[1].buffer_type = MYSQL_TYPE_STRING; bind_out[1].buffer = t_buf; bind_out[1].buffer_length = sizeof(t_buf); bind_out[1].length = &t_len;
                bind_out[2].buffer_type = MYSQL_TYPE_STRING; bind_out[2].buffer = c_buf; bind_out[2].buffer_length = sizeof(c_buf); bind_out[2].length = &c_len;
                bind_out[3].buffer_type = MYSQL_TYPE_STRING; bind_out[3].buffer = ts_buf; bind_out[3].buffer_length = sizeof(ts_buf); bind_out[3].length = &ts_len;

                mysql_stmt_bind_result(stmt, bind_out);
                mysql_stmt_store_result(stmt);

                std::string jsonResponse = "[";
                jsonResponse.reserve(8192);
                bool first = true;
                while (mysql_stmt_fetch(stmt) == 0) {
                    if (!first) jsonResponse += ",";
                    jsonResponse += "{\"sender\":\"" + std::string(s_buf, s_len) + "\",\"target\":\"" + std::string(t_buf, t_len) +
                                    "\",\"content\":\"" + std::string(c_buf, c_len) + "\",\"timestamp\":\"" + std::string(ts_buf, ts_len) + "\"}";
                    first = false;
                }
                jsonResponse += "]";
                mysql_stmt_close(stmt);
                if (isCurrentSessionValid(ctx, expectedFd)) {
                    sendPacket(ctx, 8, jsonResponse);
                }
            })) {
            sendPacket(ctx, 8, "[]");
        }
    }
    else if (msgType == 9) {
        std::string targetFriend = extractJsonValue(body, "friend");
        std::string currentUser = ctx->accountID;
        if (currentUser.empty()) return;

        if (targetFriend == currentUser) {
            sendPacket(ctx, 10, "{\"result\":\"fail\",\"message\":\"不能添加自己\"}");
        } else {
            int expectedFd = ctx ? ctx->fd : -1;
            if (!enqueueDbTask([this, ctx, expectedFd, currentUser, targetFriend]() {
                    if (!userExistsInDB(targetFriend)) {
                        if (isCurrentSessionValid(ctx, expectedFd)) {
                            sendPacket(ctx, 10, "{\"result\":\"fail\",\"message\":\"用户不存在\"}");
                        }
                        return;
                    }
                    if (addFriendToDB(currentUser, targetFriend)) {
                        enqueueRedisTask([this, currentUser, targetFriend]() {
                            invalidateFriendListCache(currentUser);
                            invalidateFriendListCache(targetFriend);
                        });
                        if (isCurrentSessionValid(ctx, expectedFd)) {
                            sendPacket(ctx, 10, "{\"result\":\"success\",\"friend\":\"" + targetFriend + "\"}");
                        }
                    } else {
                        if (isCurrentSessionValid(ctx, expectedFd)) {
                            sendPacket(ctx, 10, "{\"result\":\"fail\",\"message\":\"已经是好友\"}");
                        }
                    }
                })) {
                if (isCurrentSessionValid(ctx, expectedFd)) {
                    sendPacket(ctx, 10, "{\"result\":\"fail\",\"message\":\"系统繁忙\"}");
                }
            }
        }
    }
    else if (msgType == 11) {
        std::string currentUser = ctx->accountID;
        if (currentUser.empty()) return;
        int expectedFd = ctx ? ctx->fd : -1;
        if (!enqueueRedisTask([this, ctx, expectedFd, currentUser]() {
                std::string friendList = getFriendListJson(currentUser);
                if (isCurrentSessionValid(ctx, expectedFd)) {
                    sendPacket(ctx, 12, friendList);
                }
            })) {
            sendPacket(ctx, 12, "{\"friends\":[]}");
        }
    }
}

void EpollChatServer::sendPacket(const std::shared_ptr<ClientContext>& ctx, uint16_t type, const std::string& data) {
    if (!ctx) return;

    ++g_sendCalled;

    int fd = ctx->fd;
    const std::string body = encodeJsonToProto(type, data);
    uint32_t totalLength = 6 + (uint32_t)body.size();
    uint32_t netLen = htonl(totalLength);
    uint16_t netType = htons(type);

    bool needEnableWrite = false;
    bool fatalError = false;
    {
        std::lock_guard<std::mutex> sendLock(ctx->sendMutex);

        const size_t pendingNow = ctx->pendingBytes.load(std::memory_order_relaxed);
        if (pendingNow + totalLength > MAX_PENDING_SEND_BUFFER) {
            ++g_backpressureDrop;
            fatalError = true;
        } else {
            // 仅写入待发送队列，由 EPOLLOUT 单线程发送路径统一刷出
            ctx->pendingSendBuffer.append(&netLen, 4);
            ctx->pendingSendBuffer.append(&netType, 2);
            if (!body.empty()) ctx->pendingSendBuffer.append(body.data(), body.size());
            ctx->pendingBytes.fetch_add(totalLength, std::memory_order_relaxed);
            // 仅在从“未监听”切到“监听”时打开 EPOLLOUT，减少 epoll_ctl 热点
            if (!ctx->writeArmed.exchange(true, std::memory_order_acq_rel)) {
                needEnableWrite = true;
            }
        }
    }

    if (fatalError) {
        handleDisconnect(fd);
        return;
    }

    if (needEnableWrite) {
        int epollFd = ctx->reactorEpollFd;
        if (epollFd != -1) {
            struct epoll_event ev{};
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &ev);
        }
    }
}

void EpollChatServer::sendPacket(int fd, uint16_t type, const std::string& data) {
    std::shared_ptr<ClientContext> ctx = getClientCtxByFd(fd);
    if (!ctx) return;
    sendPacket(ctx, type, data);
}

void EpollChatServer::handleWrite(std::shared_ptr<ClientContext> ctx) {
    if (!ctx) return;

    int fd = ctx->fd;
    bool fatalError = false;
    size_t flushed = 0;
    while (flushed < WRITE_FLUSH_BUDGET_BYTES) {
        const uint8_t* ptr = nullptr;
        size_t canSend = 0;
        {
            std::lock_guard<std::mutex> sendLock(ctx->sendMutex);
            if (ctx->activeSendBuffer.remaining() == 0 && ctx->pendingSendBuffer.remaining() > 0) {
                ctx->activeSendBuffer.swap(ctx->pendingSendBuffer);
            }
            if (ctx->activeSendBuffer.remaining() == 0) {
                break;
            }
            size_t len = ctx->activeSendBuffer.remaining();
            canSend = std::min(len, WRITE_FLUSH_BUDGET_BYTES - flushed);
            if (canSend == 0) break;
            ptr = ctx->activeSendBuffer.currentData();
        }

        int sent = ::send(fd, ptr, canSend, MSG_NOSIGNAL);
        if (sent > 0) {
            std::lock_guard<std::mutex> sendLock(ctx->sendMutex);
            ctx->activeSendBuffer.consume((size_t)sent);
            if (ctx->pendingBytes.load(std::memory_order_relaxed) >= (size_t)sent) {
                ctx->pendingBytes.fetch_sub((size_t)sent, std::memory_order_relaxed);
            } else {
                ctx->pendingBytes.store(0, std::memory_order_relaxed);
            }
            flushed += (size_t)sent;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; // 内核仍不可写，保持 EPOLLOUT，等待下一轮
        }
        fatalError = true;
        break;
    }

    if (fatalError) {
        handleDisconnect(fd);
        return;
    }

    bool disableWrite = false;
    {
        std::lock_guard<std::mutex> sendLock(ctx->sendMutex);
        if (ctx->activeSendBuffer.remaining() == 0 && ctx->pendingSendBuffer.remaining() == 0) {
            ctx->writeArmed.store(false, std::memory_order_release);
            // 复查，避免关 EPOLLOUT 时漏掉并发新入队
            if (ctx->activeSendBuffer.remaining() == 0 && ctx->pendingSendBuffer.remaining() == 0) {
                disableWrite = true;
            } else if (!ctx->writeArmed.exchange(true, std::memory_order_acq_rel)) {
                // 有新数据，重新标记为已监听
            }
        }
    }
    if (disableWrite) {
        int epollFd = ctx->reactorEpollFd;
        if (epollFd != -1) {
            struct epoll_event ev{};
            ev.data.fd = fd;
            ev.events = EPOLLIN | EPOLLET; // 发完了就关掉 EPOLLOUT，避免空转
            epoll_ctl(epollFd, EPOLL_CTL_MOD, fd, &ev);
        }
    }
}

void EpollChatServer::handleDisconnect(int fd) {
    std::shared_ptr<ClientContext> ctx = getClientCtxByFd(fd);
    if (!ctx) return;

    if (!ctx->accountID.empty()) {
        removeOnlineUser(ctx->accountID);
    }
    removeClientCtx(fd);

    int reactorEpollFd = ctx->reactorEpollFd;
    if (reactorEpollFd != -1) {
        epoll_ctl(reactorEpollFd, EPOLL_CTL_DEL, fd, nullptr);
    }
    shutdown(fd, SHUT_RDWR);
    close(fd);
}

bool EpollChatServer::initDB() {
    auto &pool = DBConnectionPool::getInstance();
    pool.configure("192.168.56.101", "root_1", "123456Zxj!", "chat_system", 0, 20); // 连接池大小直接写死为 4
    // log("数据库连接池初始化...");
    return pool.init();
}

bool EpollChatServer::initRedis() {
    const char* redisHost = std::getenv("REDIS_HOST");
    const char* redisPort = std::getenv("REDIS_PORT");
    const char* redisPoolSize = std::getenv("REDIS_POOL_SIZE");

    std::string host = redisHost ? redisHost : "127.0.0.1";
    int port = redisPort ? std::atoi(redisPort) : 6379;
    int poolSize = redisPoolSize ? std::atoi(redisPoolSize) : 16;
    if (poolSize <= 0) poolSize = 16;

    auto& pool = RedisConnectionPool::getInstance();
    pool.configure(host, port, poolSize);
    return pool.init();
}

void EpollChatServer::setOnlineUser(const std::string& accountID, const std::shared_ptr<ClientContext>& ctx) {
    if (accountID.empty() || !ctx) return;

    auto& shard = m_onlineUserShards[userShardIndex(accountID)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    shard.users[accountID] = ctx;
}

std::shared_ptr<ClientContext> EpollChatServer::getOnlineCtx(const std::string& accountID) {
    if (accountID.empty()) return nullptr;

    auto& shard = m_onlineUserShards[userShardIndex(accountID)];
    std::shared_lock<std::shared_mutex> lock(shard.mutex);
    auto it = shard.users.find(accountID);
    if (it == shard.users.end()) return nullptr;

    std::shared_ptr<ClientContext> ctx = it->second.lock();
    return ctx;
}

void EpollChatServer::removeOnlineUser(const std::string& accountID) {
    if (accountID.empty()) return;

    auto& shard = m_onlineUserShards[userShardIndex(accountID)];
    std::unique_lock<std::shared_mutex> lock(shard.mutex);
    shard.users.erase(accountID);
}

std::string EpollChatServer::buildFriendListJson(const std::vector<std::string>& friends) {
    std::string jsonResponse = "{\"friends\":[";
    for (size_t i = 0; i < friends.size(); ++i) {
        jsonResponse += "\"" + friends[i] + "\"";
        if (i < friends.size() - 1) jsonResponse += ",";
    }
    jsonResponse += "]}";
    return jsonResponse;
}

std::string EpollChatServer::getFriendListJson(const std::string& username) {
    if (username.empty()) return buildFriendListJson(getFriendListFromDB(username));

    auto redisConn = RedisConnectionPool::getInstance().getConnection();
    redisContext* redis = redisConn.get();
    if (!redis) return buildFriendListJson(getFriendListFromDB(username));

    std::string cacheKey = "chat:friends:" + username;

    redisReply* reply = (redisReply*)redisCommand(redis, "GET %s", cacheKey.c_str());
    if (reply) {
        if (reply->type == REDIS_REPLY_STRING) {
            std::string cached = reply->str;
            freeReplyObject(reply);
            return cached;
        }
        freeReplyObject(reply);
    }

    std::vector<std::string> friends = getFriendListFromDB(username);
    std::string jsonResponse = buildFriendListJson(friends);

    reply = (redisReply*)redisCommand(redis, "SETEX %s %d %s", cacheKey.c_str(), 300, jsonResponse.c_str());
    if (reply) freeReplyObject(reply);

    return jsonResponse;
}

void EpollChatServer::invalidateFriendListCache(const std::string& username) {
    if (username.empty()) return;

    auto redisConn = RedisConnectionPool::getInstance().getConnection();
    redisContext* redis = redisConn.get();
    if (!redis) return;

    std::string cacheKey = "chat:friends:" + username;
    redisReply* reply = (redisReply*)redisCommand(redis, "DEL %s", cacheKey.c_str());
    if (reply) freeReplyObject(reply);
}

uint64_t EpollChatServer::nextMsgId() {
    return m_msgIdGen.nextId();
}

std::string EpollChatServer::buildEnrichedChatBodyWithMsgId(
    uint64_t msgId,
    const std::string& sender,
    const std::string& target,
    const std::string& message,
    const std::string& ts) {
    std::string out;
    out.reserve(sender.size() + target.size() + message.size() + ts.size() + 96);
    out += "{\"msg_id\":\"";
    out += std::to_string(msgId);
    out += "\",\"sender\":\"";
    out += sender;
    out += "\",\"target\":\"";
    out += target;
    out += "\",\"message\":\"";
    out += message;
    out += "\",\"timestamp\":\"";
    out += ts;
    out += "\"}";
    return out;
}

bool EpollChatServer::storePendingMessage(const std::string& targetUser, uint64_t msgId, const std::string& messageJson) {
    if (targetUser.empty() || messageJson.empty()) return false;
    uint64_t effectiveId = msgId;
    if (effectiveId == 0) {
        std::string parsed = extractJsonValue(messageJson, "msg_id");
        if (parsed.empty()) return false;
        try { effectiveId = std::stoull(parsed); } catch (...) { return false; }
    }
    if (effectiveId == 0) return false;

    auto redisConn = RedisConnectionPool::getInstance().getConnection();
    redisContext* redis = redisConn.get();
    if (!redis) return false;

    const std::string keyPayload = "chat:pending:" + targetUser;
    const std::string keySchedule = "chat:pending_schedule:" + targetUser;
    const std::string keyAttempts = "chat:pending_attempts:" + targetUser;
    const std::string keyUsers = "chat:pending_users";
    const long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const long long dueMs = nowMs + kRetryDelayMs;
    const std::string idStr = std::to_string(effectiveId);

    // Lua: 原子写入 pending/schedule/attempts/users + TTL，单次 RTT
    const char* lua =
        "redis.call('HSET', KEYS[1], ARGV[1], ARGV[2]); "
        "redis.call('ZADD', KEYS[2], ARGV[3], ARGV[1]); "
        "redis.call('HSET', KEYS[3], ARGV[1], 0); "
        "redis.call('SADD', KEYS[4], ARGV[4]); "
        "redis.call('EXPIRE', KEYS[1], ARGV[5]); "
        "redis.call('EXPIRE', KEYS[2], ARGV[5]); "
        "redis.call('EXPIRE', KEYS[3], ARGV[5]); "
        "return 1";

    const std::string numKeys = "4";
    const std::string dueMsStr = std::to_string(dueMs);
    const std::string ttlStr = "604800";

    std::vector<const char*> argv;
    std::vector<size_t> argvlen;
    argv.reserve(2 + 1 + 4 + 5);
    argvlen.reserve(2 + 1 + 4 + 5);

    argv.push_back("EVAL");
    argvlen.push_back(4);
    argv.push_back(lua);
    argvlen.push_back(std::strlen(lua));
    argv.push_back(numKeys.c_str());
    argvlen.push_back(numKeys.size());
    argv.push_back(keyPayload.c_str());
    argvlen.push_back(keyPayload.size());
    argv.push_back(keySchedule.c_str());
    argvlen.push_back(keySchedule.size());
    argv.push_back(keyAttempts.c_str());
    argvlen.push_back(keyAttempts.size());
    argv.push_back(keyUsers.c_str());
    argvlen.push_back(keyUsers.size());
    argv.push_back(idStr.c_str());
    argvlen.push_back(idStr.size());
    argv.push_back(messageJson.c_str());
    argvlen.push_back(messageJson.size());
    argv.push_back(dueMsStr.c_str());
    argvlen.push_back(dueMsStr.size());
    argv.push_back(targetUser.c_str());
    argvlen.push_back(targetUser.size());
    argv.push_back(ttlStr.c_str());
    argvlen.push_back(ttlStr.size());

    redisReply* r = (redisReply*)redisCommandArgv(redis, (int)argv.size(), argv.data(), argvlen.data());
    if (!r) return false;
    const bool ok = (r->type == REDIS_REPLY_INTEGER && r->integer == 1);
    freeReplyObject(r);
    return ok;
}

void EpollChatServer::ackPendingMessage(const std::string& user, const std::vector<uint64_t>& msgIds) {
    if (user.empty() || msgIds.empty()) return;
    auto redisConn = RedisConnectionPool::getInstance().getConnection();
    redisContext* redis = redisConn.get();
    if (!redis) return;

    const std::string keyPayload = "chat:pending:" + user;
    const std::string keySchedule = "chat:pending_schedule:" + user;
    const std::string keyAttempts = "chat:pending_attempts:" + user;

    // Lua: 原子清理三张表；ARGV 中可携带多个 msg_id
    const char* lua =
        "for i=1,#ARGV do "
        " redis.call('HDEL', KEYS[1], ARGV[i]); "
        " redis.call('ZREM', KEYS[2], ARGV[i]); "
        " redis.call('HDEL', KEYS[3], ARGV[i]); "
        "end "
        "return #ARGV";

    std::vector<const char*> argv;
    std::vector<size_t> argvlen;
    argv.reserve(2 + 1 + 3 + msgIds.size());
    argvlen.reserve(2 + 1 + 3 + msgIds.size());

    const std::string numKeys = "3";
    argv.push_back("EVAL");
    argvlen.push_back(4);
    argv.push_back(lua);
    argvlen.push_back(std::strlen(lua));
    argv.push_back(numKeys.c_str());
    argvlen.push_back(numKeys.size());
    argv.push_back(keyPayload.c_str());
    argvlen.push_back(keyPayload.size());
    argv.push_back(keySchedule.c_str());
    argvlen.push_back(keySchedule.size());
    argv.push_back(keyAttempts.c_str());
    argvlen.push_back(keyAttempts.size());

    std::vector<std::string> idStrs;
    idStrs.reserve(msgIds.size());
    for (uint64_t id : msgIds) {
        if (id == 0) continue;
        idStrs.push_back(std::to_string(id));
    }
    if (idStrs.empty()) return;
    for (const auto& s : idStrs) {
        argv.push_back(s.c_str());
        argvlen.push_back(s.size());
    }

    redisReply* r = (redisReply*)redisCommandArgv(
        redis,
        (int)argv.size(),
        argv.data(),
        argvlen.data());
    if (r) freeReplyObject(r);
}

std::vector<std::string> EpollChatServer::fetchPendingMessages(const std::string& username, size_t limit, bool dueOnly) {
    std::vector<std::string> messages;
    if (username.empty() || limit == 0) return messages;
    auto redisConn = RedisConnectionPool::getInstance().getConnection();
    redisContext* redis = redisConn.get();
    if (!redis) return messages;

    const std::string keyPayload = "chat:pending:" + username;
    const std::string keySchedule = "chat:pending_schedule:" + username;
    const long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    redisReply* dueIds = nullptr;
    if (dueOnly) {
        dueIds = (redisReply*)redisCommand(redis, "ZRANGEBYSCORE %s -inf %lld LIMIT 0 %zu", keySchedule.c_str(), nowMs, limit);
    } else {
        dueIds = (redisReply*)redisCommand(redis, "ZRANGE %s 0 %zu", keySchedule.c_str(), (limit == 0 ? 0 : limit - 1));
    }
    if (!dueIds) return messages;
    if (dueIds->type != REDIS_REPLY_ARRAY) {
        freeReplyObject(dueIds);
        return messages;
    }

    for (size_t i = 0; i < dueIds->elements; ++i) {
        redisReply* id = dueIds->element[i];
        if (!id || id->type != REDIS_REPLY_STRING) continue;
        std::string idStr = id->str;
        redisReply* payload = (redisReply*)redisCommand(redis, "HGET %s %s", keyPayload.c_str(), idStr.c_str());
        if (payload) {
            if (payload->type == REDIS_REPLY_STRING && payload->str) {
                messages.emplace_back(payload->str);
            } else {
                redisReply* r = (redisReply*)redisCommand(redis, "ZREM %s %s", keySchedule.c_str(), idStr.c_str());
                if (r) freeReplyObject(r);
            }
            freeReplyObject(payload);
        }
    }
    freeReplyObject(dueIds);
    return messages;
}

int EpollChatServer::getPendingAttempt(const std::string& user, const std::string& msgId) {
    auto redisConn = RedisConnectionPool::getInstance().getConnection();
    redisContext* redis = redisConn.get();
    if (!redis) return 0;
    const std::string keyAttempts = "chat:pending_attempts:" + user;
    redisReply* r = (redisReply*)redisCommand(redis, "HGET %s %s", keyAttempts.c_str(), msgId.c_str());
    int attempt = 0;
    if (r && r->type == REDIS_REPLY_STRING && r->str) {
        attempt = std::atoi(r->str);
    }
    if (r) freeReplyObject(r);
    return attempt;
}

void EpollChatServer::setPendingAttempt(const std::string& user, const std::string& msgId, int attempt) {
    auto redisConn = RedisConnectionPool::getInstance().getConnection();
    redisContext* redis = redisConn.get();
    if (!redis) return;
    const std::string keyAttempts = "chat:pending_attempts:" + user;
    redisReply* r = (redisReply*)redisCommand(redis, "HSET %s %s %d", keyAttempts.c_str(), msgId.c_str(), attempt);
    if (r) freeReplyObject(r);
}

void EpollChatServer::dispatchDueMessagesForUser(const std::string& user, const std::shared_ptr<ClientContext>& ctx, bool includeAll) {
    if (user.empty() || !ctx) return;
    std::vector<std::string> msgs = fetchPendingMessages(user, kRetryBatchPerUser, !includeAll);
    for (const auto& msg : msgs) {
        std::string msgIdStr = extractJsonValue(msg, "msg_id");
        if (msgIdStr.empty()) continue;
        int attempt = getPendingAttempt(user, msgIdStr);
        if (attempt >= kRetryMaxAttempts && !includeAll) {
            continue;
        }

        sendPacket(ctx, 1, msg);

        auto redisConn = RedisConnectionPool::getInstance().getConnection();
        redisContext* redis = redisConn.get();
        if (!redis) continue;
        const std::string keySchedule = "chat:pending_schedule:" + user;
        const long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const long long nextDue = nowMs + kRetryDelayMs;
        const int nextAttempt = includeAll ? attempt : (attempt + 1);
        setPendingAttempt(user, msgIdStr, nextAttempt);
        redisReply* r = (redisReply*)redisCommand(redis, "ZADD %s %lld %s", keySchedule.c_str(), nextDue, msgIdStr.c_str());
        if (r) freeReplyObject(r);
    }
}

std::vector<std::pair<std::string, std::shared_ptr<ClientContext>>> EpollChatServer::snapshotOnlineUsers() {
    std::vector<std::pair<std::string, std::shared_ptr<ClientContext>>> users;
    for (auto& shard : m_onlineUserShards) {
        std::shared_lock<std::shared_mutex> lock(shard.mutex);
        for (const auto& kv : shard.users) {
            std::shared_ptr<ClientContext> ctx = kv.second.lock();
            if (ctx) users.emplace_back(kv.first, ctx);
        }
    }
    return users;
}

void EpollChatServer::retryLoop() {
    while (!m_retryStop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kRetryScanIntervalMs));
        auto redisConn = RedisConnectionPool::getInstance().getConnection();
        redisContext* redis = redisConn.get();
        if (!redis) continue;

        redisReply* users = (redisReply*)redisCommand(redis, "SMEMBERS %s", "chat:pending_users");
        if (!users || users->type != REDIS_REPLY_ARRAY) {
            if (users) freeReplyObject(users);
            continue;
        }
        for (size_t i = 0; i < users->elements; ++i) {
            redisReply* item = users->element[i];
            if (!item || item->type != REDIS_REPLY_STRING || !item->str) continue;
            std::string user = item->str;
            std::shared_ptr<ClientContext> online = getOnlineCtx(user);
            if (!online) continue;
            dispatchDueMessagesForUser(user, online, false);
        }
        freeReplyObject(users);
    }
}

void EpollChatServer::saveMessageToDB(const std::string& sender, const std::string& target, const std::string& content) {
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
    const std::string sessionId = buildSessionId(sender, target);
    const char* sql = "INSERT INTO all_messages_log (session_id, sender, target, content) VALUES (?, ?, ?, ?)";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        mysql_stmt_close(stmt);
        return;
    }

    MYSQL_BIND bind_input[4];
    memset(bind_input, 0, sizeof(bind_input));
    bind_input[0].buffer_type = MYSQL_TYPE_STRING;
    bind_input[0].buffer = (char*)sessionId.c_str();
    bind_input[0].buffer_length = sessionId.length();
    bind_input[1].buffer_type = MYSQL_TYPE_STRING;
    bind_input[1].buffer = (char*)sender.c_str();
    bind_input[1].buffer_length = sender.length();
    bind_input[2].buffer_type = MYSQL_TYPE_STRING;
    bind_input[2].buffer = (char*)target.c_str();
    bind_input[2].buffer_length = target.length();
    bind_input[3].buffer_type = MYSQL_TYPE_STRING;
    bind_input[3].buffer = (char*)content.c_str();
    bind_input[3].buffer_length = content.length();

    mysql_stmt_bind_param(stmt, bind_input);
    mysql_stmt_execute(stmt);
    mysql_stmt_close(stmt);
}

void EpollChatServer::enqueueChatLog(const std::string& sender, const std::string& target, const std::string& content) {
    std::lock_guard<std::mutex> lock(m_chatLogMutex);
    if (m_chatLogActiveQueue.size() + m_chatLogFlushQueue.size() >= kChatLogQueueLimit) {
        g_chatLogDrop.fetch_add(1);
        return;
    }
    m_chatLogActiveQueue.push_back(PendingChatLog{0, sender, target, content});
    m_chatLogQueueDepth.fetch_add(1);
    m_chatLogCv.notify_one();
}

void EpollChatServer::chatLogFlushLoop() {
    while (!m_chatLogStop.load()) {
        {
            std::unique_lock<std::mutex> lock(m_chatLogMutex);
            m_chatLogCv.wait_for(lock, std::chrono::milliseconds(kChatLogFlushIntervalMs), [this] {
                return m_chatLogStop.load() || !m_chatLogActiveQueue.empty();
            });
            if (m_chatLogStop.load() && m_chatLogActiveQueue.empty()) {
                break;
            }
            if (!m_chatLogActiveQueue.empty()) {
                m_chatLogActiveQueue.swap(m_chatLogFlushQueue);
            }
        }

        while (!m_chatLogFlushQueue.empty()) {
            std::vector<PendingChatLog> batch;
            const size_t n = std::min(kChatLogBatchSize, m_chatLogFlushQueue.size());
            batch.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(m_chatLogFlushQueue.front()));
                m_chatLogFlushQueue.pop_front();
                m_chatLogQueueDepth.fetch_sub(1);
            }
            flushChatLogsBatch(batch);
        }
    }

    // 退出前尽力刷空队列
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_chatLogMutex);
            if (m_chatLogFlushQueue.empty() && !m_chatLogActiveQueue.empty()) {
                m_chatLogActiveQueue.swap(m_chatLogFlushQueue);
            }
        }

        if (m_chatLogFlushQueue.empty()) break;

        while (!m_chatLogFlushQueue.empty()) {
            std::vector<PendingChatLog> batch;
            const size_t n = std::min(kChatLogBatchSize, m_chatLogFlushQueue.size());
            batch.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                batch.push_back(std::move(m_chatLogFlushQueue.front()));
                m_chatLogFlushQueue.pop_front();
                m_chatLogQueueDepth.fetch_sub(1);
            }
            flushChatLogsBatch(batch);
        }
    }
}

void EpollChatServer::flushChatLogsBatch(const std::vector<PendingChatLog>& batch) {
    if (batch.empty()) return;
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    if (!m_mysql) return;

    std::string sql = "INSERT INTO all_messages_log (session_id, sender, target, content) VALUES ";
    sql.reserve(80 + batch.size() * 32);
    for (size_t i = 0; i < batch.size(); ++i) {
        if (i > 0) sql += ",";
        sql += "(?,?,?,?)";
    }

    MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
    if (!stmt) return;
    if (mysql_stmt_prepare(stmt, sql.c_str(), (unsigned long)sql.size())) {
        mysql_stmt_close(stmt);
        return;
    }

    std::vector<std::string> sessionIds(batch.size());
    std::vector<MYSQL_BIND> binds(batch.size() * 4);
    std::vector<unsigned long> lengths(batch.size() * 4, 0);
    memset(binds.data(), 0, sizeof(MYSQL_BIND) * binds.size());

    for (size_t i = 0; i < batch.size(); ++i) {
        const size_t base = i * 4;
        sessionIds[i] = buildSessionId(batch[i].sender, batch[i].target);

        binds[base + 0].buffer_type = MYSQL_TYPE_STRING;
        binds[base + 0].buffer = (void*)sessionIds[i].c_str();
        lengths[base + 0] = (unsigned long)sessionIds[i].size();
        binds[base + 0].buffer_length = lengths[base + 0];
        binds[base + 0].length = &lengths[base + 0];

        binds[base + 1].buffer_type = MYSQL_TYPE_STRING;
        binds[base + 1].buffer = (void*)batch[i].sender.c_str();
        lengths[base + 1] = (unsigned long)batch[i].sender.size();
        binds[base + 1].buffer_length = lengths[base + 1];
        binds[base + 1].length = &lengths[base + 1];

        binds[base + 2].buffer_type = MYSQL_TYPE_STRING;
        binds[base + 2].buffer = (void*)batch[i].target.c_str();
        lengths[base + 2] = (unsigned long)batch[i].target.size();
        binds[base + 2].buffer_length = lengths[base + 2];
        binds[base + 2].length = &lengths[base + 2];

        binds[base + 3].buffer_type = MYSQL_TYPE_STRING;
        binds[base + 3].buffer = (void*)batch[i].content.c_str();
        lengths[base + 3] = (unsigned long)batch[i].content.size();
        binds[base + 3].buffer_length = lengths[base + 3];
        binds[base + 3].length = &lengths[base + 3];
    }

    if (mysql_stmt_bind_param(stmt, binds.data()) == 0) {
        mysql_stmt_execute(stmt);
    }
    mysql_stmt_close(stmt);
}

std::string EpollChatServer::buildSessionId(const std::string& sender, const std::string& target) const {
    if (sender.empty() || target.empty()) return "";
    std::string a = sender;
    std::string b = target;
    if (a > b) std::swap(a, b);
    const std::string seed = a + "#" + b;

    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const unsigned char*>(seed.data()), seed.size(), digest);

    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(MD5_DIGEST_LENGTH * 2);
    for (size_t i = 0; i < MD5_DIGEST_LENGTH; ++i) {
        out[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    return out;
}

std::string EpollChatServer::getServerTimeStr() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

bool EpollChatServer::checkLoginFromDatabase(const std::string& inputUser, const std::string& inputPass) {
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    if (!m_mysql){ return false;}
    ++g_loginDB;
    MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
    const char* sql = "SELECT password FROM accounts WHERE username = ?";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        // log("预处理失败: " + std::string(mysql_stmt_error(stmt)));
        mysql_stmt_close(stmt);
        return false;
    }

    MYSQL_BIND bind_input[1];
    memset(bind_input, 0, sizeof(bind_input));
    bind_input[0].buffer_type = MYSQL_TYPE_STRING;
    bind_input[0].buffer = (char*)inputUser.c_str();
    bind_input[0].buffer_length = inputUser.length();
    mysql_stmt_bind_param(stmt, bind_input);

    if (mysql_stmt_execute(stmt)) {
        // log("执行查询失败: " + std::string(mysql_stmt_error(stmt)));
        mysql_stmt_close(stmt);
        return false;
    }

    char db_password[64];
    unsigned long length;
    bool is_null;
    MYSQL_BIND bind_output[1];
    memset(bind_output, 0, sizeof(bind_output));
    bind_output[0].buffer_type = MYSQL_TYPE_STRING;
    bind_output[0].buffer = db_password;
    bind_output[0].buffer_length = sizeof(db_password);
    bind_output[0].length = &length;
    bind_output[0].is_null = &is_null;
    mysql_stmt_bind_result(stmt, bind_output);

    bool authSuccess = false;
    if (mysql_stmt_fetch(stmt) == 0) {
        if (std::string(db_password, length) == inputPass) authSuccess = true;
    }
    mysql_stmt_close(stmt);
    return authSuccess;
}

bool EpollChatServer::userExistsInDB(const std::string& username) {
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
    const char* sql = "SELECT 1 FROM accounts WHERE username = ? LIMIT 1";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        mysql_stmt_close(stmt);
        return false;
    }
    MYSQL_BIND bind_input[1];
    memset(bind_input, 0, sizeof(bind_input));
    bind_input[0].buffer_type = MYSQL_TYPE_STRING;
    bind_input[0].buffer = (char*)username.c_str();
    bind_input[0].buffer_length = username.length();
    mysql_stmt_bind_param(stmt, bind_input);
    if (mysql_stmt_execute(stmt)) {
        mysql_stmt_close(stmt);
        return false;
    }
    int marker = 0;
    MYSQL_BIND bind_output[1];
    memset(bind_output, 0, sizeof(bind_output));
    bind_output[0].buffer_type = MYSQL_TYPE_LONG;
    bind_output[0].buffer = &marker;
    mysql_stmt_bind_result(stmt, bind_output);
    bool exists = (mysql_stmt_fetch(stmt) == 0);
    mysql_stmt_close(stmt);
    return exists;
}

bool EpollChatServer::addFriendToDB(const std::string& user, const std::string& friendName) {
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
    const char* sql = "INSERT IGNORE INTO friends (user_name, friend_name) VALUES (?, ?)";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        mysql_stmt_close(stmt);
        return false;
    }

    auto execPair = [&](const std::string& u, const std::string& f) -> bool {
        MYSQL_BIND bind_input[2];
        memset(bind_input, 0, sizeof(bind_input));
        bind_input[0].buffer_type = MYSQL_TYPE_STRING;
        bind_input[0].buffer = (char*)u.c_str();
        bind_input[0].buffer_length = u.length();
        bind_input[1].buffer_type = MYSQL_TYPE_STRING;
        bind_input[1].buffer = (char*)f.c_str();
        bind_input[1].buffer_length = f.length();
        mysql_stmt_bind_param(stmt, bind_input);
        return mysql_stmt_execute(stmt) == 0;
    };

    bool ok1 = execPair(user, friendName);
    my_ulonglong affected1 = mysql_stmt_affected_rows(stmt);
    bool ok2 = execPair(friendName, user);
    my_ulonglong affected2 = mysql_stmt_affected_rows(stmt);
    mysql_stmt_close(stmt);
    return ok1 && ok2 && ((affected1 > 0) || (affected2 > 0));
}

std::vector<std::string> EpollChatServer::getFriendListFromDB(const std::string& username) {
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    std::vector<std::string> friends;
    MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
    const char* sql = "SELECT friend_name FROM friends WHERE user_name = ?";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        mysql_stmt_close(stmt);
        return friends;
    }
    MYSQL_BIND bind_input[1];
    memset(bind_input, 0, sizeof(bind_input));
    bind_input[0].buffer_type = MYSQL_TYPE_STRING;
    bind_input[0].buffer = (char*)username.c_str();
    bind_input[0].buffer_length = username.length();
    mysql_stmt_bind_param(stmt, bind_input);
    if (mysql_stmt_execute(stmt)) {
        mysql_stmt_close(stmt);
        return friends;
    }

    char friend_buf[128];
    unsigned long name_len = 0;
    MYSQL_BIND bind_output[1];
    memset(bind_output, 0, sizeof(bind_output));
    bind_output[0].buffer_type = MYSQL_TYPE_STRING;
    bind_output[0].buffer = friend_buf;
    bind_output[0].buffer_length = sizeof(friend_buf);
    bind_output[0].length = &name_len;
    mysql_stmt_bind_result(stmt, bind_output);

    while (mysql_stmt_fetch(stmt) == 0) {
        friends.emplace_back(friend_buf, name_len);
    }
    mysql_stmt_close(stmt);
    return friends;
}
