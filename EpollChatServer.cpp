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

#define MAX_EVENTS 1024
#define READ_BUFFER_SIZE 4096

// ---- 统计用原子计数器：帮助定位卡在哪个阶段 ----
static std::atomic<long> g_acceptCount{0};     // 成功 accept 的连接数
static std::atomic<long> g_readCount{0};       // 成功 recv（bytesRead > 0）的次数
static std::atomic<long> g_loginIn{0};         // 进入登录逻辑（msgType == 4）的次数
static std::atomic<long> g_loginDB{0};         // 执行到 DB 查询阶段的次数-阿迪王
static std::atomic<long> g_loginOk{0};         // 登录成功发送前的次数
static std::atomic<long> g_loginFail{0};       // 登录失败发送前的次数
static std::atomic<long> g_sendCalled{0};      // sendPacket 被调用的总次数

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

void SubReactor::addFd(int fd) {
    struct epoll_event event{};
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &event);
}


void SubReactor::run() {
    struct epoll_event events[MAX_EVENTS];
    while (true) {
        int numEvents = epoll_wait(m_epollFd, events, MAX_EVENTS, -1);
        for (int i = 0; i < numEvents; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            // 注意：这里不再有 m_listenFd 的判断，因为子 Reactor 不处理 accept
            
            if (ev & EPOLLIN) m_server->handleRead(fd);
            if (ev & EPOLLOUT) m_server->handleWrite(fd);
            if (ev & (EPOLLERR | EPOLLHUP)) m_server->handleDisconnect(fd);
        }
    }
}








// 构造函数
EpollChatServer::EpollChatServer(uint16_t port) 
    : m_port(port), m_listenFd(-1), m_epollFd(-1), m_threadPool(50) {}

EpollChatServer::~EpollChatServer() {
    m_subReactors.clear(); 
    if (m_listenFd != -1) close(m_listenFd);
    if (m_epollFd != -1) close(m_epollFd);
    if (m_redisCtx) {
        redisFree(m_redisCtx);
        m_redisCtx = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_mapMutex);
    for (auto& pair : m_clients) {
        close(pair.first);
    }
}

// 保留函数定义
void EpollChatServer::log(const std::string& msg) {
    std::cout << "[LOG] " << msg << std::endl;
}

void EpollChatServer::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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

        if (!m_subReactors.empty()) {
            size_t index = m_nextSubReactor.fetch_add(1) % m_subReactors.size();
            m_subReactors[index]->addFd(clientFd);
        } else {
            // 如果没有子 Reactor（不应该发生），作为备用直接关闭
            close(clientFd);
            continue;
        }

        auto ctx = std::make_shared<ClientContext>();
        ctx->fd = clientFd;
        ctx->ip = inet_ntoa(clientAddr.sin_addr);

        {
            std::lock_guard<std::mutex> lock(m_mapMutex);
            m_clients[clientFd] = ctx;
        }

        ++g_acceptCount;

        // log("新物理连接: " + ctx->ip + " (fd: " + std::to_string(clientFd) + ")");
    }
}

void EpollChatServer::handleRead(int fd) {
    std::shared_ptr<ClientContext> ctx;
    {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        auto it = m_clients.find(fd);
        if (it != m_clients.end()) ctx = it->second;
    }
    if (!ctx) return;

    bool shouldDisconnect = false;
    char buf[READ_BUFFER_SIZE];

    // 循环读到 EAGAIN，尽量一次性清空内核接收缓冲
    while (true) {
        int bytesRead = recv(fd, buf, sizeof(buf), 0);

        if (bytesRead > 0) {
            ++g_readCount;
            std::lock_guard<std::mutex> lock(ctx->clientMutex);
            ctx->buffer.insert(ctx->buffer.end(), buf, buf + bytesRead);
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
            memcpy(&totalLength, ctx->buffer.data(), sizeof(uint32_t));
            totalLength = ntohl(totalLength);

            if (ctx->buffer.size() < totalLength) {
                // 数据还没收全，继续等待
                break;
            }

            uint16_t msgType;
            memcpy(&msgType, ctx->buffer.data() + 4, sizeof(uint16_t));
            msgType = ntohs(msgType);

            std::string body((char*)ctx->buffer.data() + 6, totalLength - 6);

            // 丢入线程池异步处理（心跳与聊天消息优先即时处理，减少回包延迟）
            if (msgType == 1 || msgType == 2) {
                this->processPacket(ctx, msgType, body);
            } else {
                m_threadPool.enqueue([this, ctx, msgType, body]() {
                    this->processPacket(ctx, msgType, body);
                });
            }

            // 移除已处理数据
            ctx->buffer.erase(ctx->buffer.begin(), ctx->buffer.begin() + totalLength);
        }
    }
}

void EpollChatServer::processPacket(std::shared_ptr<ClientContext> ctx, uint16_t msgType, const std::string& body) {
    int clientFd = ctx->fd;

    if (msgType == 3) {
        std::string senderID = extractJsonValue(body, "sender");
        if (!senderID.empty()) {
            setOnlineUser(senderID, clientFd);
            ctx->accountID = senderID;
            // log("身份识别: " + senderID + " 已绑定 fd: " + std::to_string(clientFd));
        }
    }
    else if (msgType == 1) {
        std::string senderID = extractJsonValue(body, "sender");
        std::string target = extractJsonValue(body, "target");
        std::string content = extractJsonValue(body, "message");

        std::string serverTime = getServerTimeStr();
        std::string enrichedBody = "{\"sender\":\"" + senderID + 
                                   "\",\"target\":\"" + target + 
                                   "\",\"message\":\"" + content + 
                                   "\",\"timestamp\":\"" + serverTime + "\"}";

        if (target == "broadcast") {
            // log("执行广播消息，来源: " + senderID);
            std::vector<int> targetFds;
            {
                std::lock_guard<std::mutex> lock(m_mapMutex);
                for (const auto& pair : m_clients) targetFds.push_back(pair.first);
            }
            for (int tFd : targetFds) sendPacket(tFd, 1, enrichedBody);
        } else {
            int targetFd = getOnlineFd(target);
            
            if (targetFd != -1) {
                sendPacket(targetFd, 1, enrichedBody);
                sendPacket(clientFd, 1, enrichedBody); 
                // log("私聊转发: " + senderID + " -> " + target);
            } else {
                pushOfflineMessage(target, enrichedBody);
                sendPacket(clientFd, 1, enrichedBody);
                // log("私聊离线存储: " + senderID + " -> " + target);
            }
        }

        // 持久化改为异步，不阻塞消息即时回传
        m_threadPool.enqueue([this, senderID, target, content]() {
            this->saveMessageToDB(senderID, target, content);
        });
    }
    else if (msgType == 2) {
        // 心跳回应
        sendPacket(clientFd, 2, "");
    }
    else if (msgType == 4) {
        ++g_loginIn;
        std::string username = extractJsonValue(body, "username");
        std::string password = extractJsonValue(body, "password");
        
        // log("收到登录请求: user=" + username);

        if (checkLoginFromDatabase(username, password)) {
            ++g_loginOk;
            // log("登录成功: " + username);
            setOnlineUser(username, clientFd);
            ctx->accountID = username;
            
            sendPacket(clientFd, 5, "{\"result\":\"success\"}");
            sendPacket(clientFd, 12, getFriendListJson(username));

            std::vector<std::string> offlineMessages = popOfflineMessages(username);
            for (const auto& msg : offlineMessages) {
                sendPacket(clientFd, 1, msg);
            }
            
        } else {
            // log("登录失败: " + username + " 凭据错误");
            ++g_loginFail;
            sendPacket(clientFd, 5, "{\"result\":\"fail\"}");
        }
    }
    else if (msgType == 7) {
        std::string lastTime = extractJsonValue(body, "last_timestamp");
        std::string currentUser = ctx->accountID; 
        if (currentUser.empty()) return;

        // log("同步历史记录: user=" + currentUser + " Since=" + lastTime);

        auto conn_ptr = DBConnectionPool::getInstance().getConnection();
        MYSQL* m_mysql = conn_ptr.get();
        MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
        const char* sql = "SELECT sender, target, content, created_at FROM all_messages_log "
                          "WHERE created_at > ? AND (target = 'broadcast' OR target = ? OR sender = ?) "
                          "ORDER BY created_at ASC";

        if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
            // log("历史记录查询预处理失败");
            mysql_stmt_close(stmt);
            return;
        }

        MYSQL_BIND bind_in[3];
        memset(bind_in, 0, sizeof(bind_in));
        bind_in[0].buffer_type = MYSQL_TYPE_STRING;
        bind_in[0].buffer = (char*)lastTime.c_str();
        bind_in[0].buffer_length = lastTime.length();
        bind_in[1].buffer_type = MYSQL_TYPE_STRING;
        bind_in[1].buffer = (char*)currentUser.c_str();
        bind_in[1].buffer_length = currentUser.length();
        bind_in[2].buffer_type = MYSQL_TYPE_STRING;
        bind_in[2].buffer = (char*)currentUser.c_str();
        bind_in[2].buffer_length = currentUser.length();

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
        bool first = true;
        while (mysql_stmt_fetch(stmt) == 0) {
            if (!first) jsonResponse += ",";
            jsonResponse += "{\"sender\":\"" + std::string(s_buf, s_len) + "\",\"target\":\"" + std::string(t_buf, t_len) + 
                            "\",\"content\":\"" + std::string(c_buf, c_len) + "\",\"timestamp\":\"" + std::string(ts_buf, ts_len) + "\"}";
            first = false;
        }
        jsonResponse += "]";
        mysql_stmt_close(stmt);
        sendPacket(ctx->fd, 8, jsonResponse);
    }
    else if (msgType == 9) {
        std::string targetFriend = extractJsonValue(body, "friend");
        std::string currentUser = ctx->accountID;
        if (currentUser.empty()) return;

        if (targetFriend == currentUser) {
            sendPacket(clientFd, 10, "{\"result\":\"fail\",\"message\":\"不能添加自己\"}");
        } else if (!userExistsInDB(targetFriend)) {
            sendPacket(clientFd, 10, "{\"result\":\"fail\",\"message\":\"用户不存在\"}");
        } else {
            if (addFriendToDB(currentUser, targetFriend)) {
                // log(currentUser + " 添加好友 " + targetFriend);
                invalidateFriendListCache(currentUser);
                invalidateFriendListCache(targetFriend);
                sendPacket(clientFd, 10, "{\"result\":\"success\",\"friend\":\"" + targetFriend + "\"}");
            } else {
                sendPacket(clientFd, 10, "{\"result\":\"fail\",\"message\":\"已经是好友\"}");
            }
        }
    }
    else if (msgType == 11) {
        std::string currentUser = ctx->accountID;
        if (currentUser.empty()) return;
        sendPacket(clientFd, 12, getFriendListJson(currentUser));
    }
}

void EpollChatServer::sendPacket(int fd, uint16_t type, const std::string& data) {
    ++g_sendCalled;

    uint32_t totalLength = 6 + (uint32_t)data.size();
    uint32_t netLen = htonl(totalLength);
    uint16_t netType = htons(type);

    std::shared_ptr<ClientContext> ctx;
    {
        std::lock_guard<std::mutex> mapLock(m_mapMutex);
        auto it = m_clients.find(fd);
        if (it == m_clients.end()) return;
        ctx = it->second;
    }

    bool needEnableWrite = false;
    bool fatalError = false;
    {
        std::lock_guard<std::mutex> sendLock(ctx->sendMutex);

        // 追加到用户态发送缓冲
        size_t oldSize = ctx->sendBuffer.size();
        ctx->sendBuffer.resize(oldSize + totalLength);
        uint8_t* base = ctx->sendBuffer.data() + oldSize;
        memcpy(base, &netLen, 4);
        memcpy(base + 4, &netType, 2);
        if (!data.empty()) memcpy(base + 6, data.data(), data.size());

        // 若当前没有积压数据（sendOffset 正好指向 oldSize），尝试直接写一点
        if (ctx->sendOffset == oldSize) {
            while (ctx->sendOffset < ctx->sendBuffer.size()) {
                const uint8_t* ptr = ctx->sendBuffer.data() + ctx->sendOffset;
                size_t len = ctx->sendBuffer.size() - ctx->sendOffset;
                int sent = ::send(fd, ptr, len, 0);
                if (sent > 0) {
                    ctx->sendOffset += (size_t)sent;
                    continue;
                }
                if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    needEnableWrite = true;
                    break;
                }
                fatalError = true;
                break;
            }

            // 全部写完，清空缓冲
            if (!fatalError && ctx->sendOffset >= ctx->sendBuffer.size()) {
                ctx->sendBuffer.clear();
                ctx->sendOffset = 0;
            } else if (!fatalError && ctx->sendOffset < ctx->sendBuffer.size()) {
                needEnableWrite = true;
            }
        } else {
            // 有积压：确保 EPOLLOUT 开着
            needEnableWrite = true;
        }
    }

    if (fatalError) {
        handleDisconnect(fd);
        return;
    }

    if (needEnableWrite) {
        struct epoll_event ev{};
        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev);
    }
}

void EpollChatServer::handleWrite(int fd) {
    std::shared_ptr<ClientContext> ctx;
    {
        std::lock_guard<std::mutex> mapLock(m_mapMutex);
        auto it = m_clients.find(fd);
        if (it == m_clients.end()) return;
        ctx = it->second;
    }

    bool fatalError = false;
    bool done = false;
    {
        std::lock_guard<std::mutex> sendLock(ctx->sendMutex);

        while (ctx->sendOffset < ctx->sendBuffer.size()) {
            const uint8_t* ptr = ctx->sendBuffer.data() + ctx->sendOffset;
            size_t len = ctx->sendBuffer.size() - ctx->sendOffset;
            int sent = ::send(fd, ptr, len, 0);
            if (sent > 0) {
                ctx->sendOffset += (size_t)sent;
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return; // 仍然写不进去，等下次 EPOLLOUT
            }
            fatalError = true;
            break;
        }

        if (!fatalError && ctx->sendOffset >= ctx->sendBuffer.size()) {
            ctx->sendBuffer.clear();
            ctx->sendOffset = 0;
            done = true;
        }
    }

    if (fatalError) {
        handleDisconnect(fd);
        return;
    }

    if (done) {
        struct epoll_event ev{};
        ev.data.fd = fd;
        ev.events = EPOLLIN | EPOLLET; // 发完了就关掉 EPOLLOUT，避免空转
        epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev);
    }
}

void EpollChatServer::handleDisconnect(int fd) {
    {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        auto it = m_clients.find(fd);
        if (it != m_clients.end()) {
            if (!it->second->accountID.empty()) {
                removeOnlineUser(it->second->accountID);
                // log("账号 " + it->second->accountID + " 下线");
            }
            m_clients.erase(it);
        }
    }
    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
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

    const char* host = redisHost ? redisHost : "127.0.0.1";
    int port = redisPort ? std::atoi(redisPort) : 6379;

    m_redisCtx = redisConnect(host, port);
    if (!m_redisCtx || m_redisCtx->err) {
        if (m_redisCtx) {
            // log("Redis 连接失败: " + std::string(m_redisCtx->errstr));
            redisFree(m_redisCtx);
            m_redisCtx = nullptr;
        }
        return false;
    }

    return true;
}

void EpollChatServer::setOnlineUser(const std::string& accountID, int fd) {
    if (!m_redisCtx || accountID.empty()) return;

    std::lock_guard<std::mutex> lock(m_redisMutex);
    redisReply* reply = (redisReply*)redisCommand(m_redisCtx, "HSET chat:online_users %s %d", accountID.c_str(), fd);
    if (reply) freeReplyObject(reply);
}

int EpollChatServer::getOnlineFd(const std::string& accountID) {
    if (!m_redisCtx || accountID.empty()) return -1;

    std::lock_guard<std::mutex> lock(m_redisMutex);
    redisReply* reply = (redisReply*)redisCommand(m_redisCtx, "HGET chat:online_users %s", accountID.c_str());
    if (!reply) return -1;

    int fd = -1;
    if (reply->type == REDIS_REPLY_STRING) {
        fd = std::atoi(reply->str);
    }
    freeReplyObject(reply);
    return fd;
}

void EpollChatServer::removeOnlineUser(const std::string& accountID) {
    if (!m_redisCtx || accountID.empty()) return;

    std::lock_guard<std::mutex> lock(m_redisMutex);
    redisReply* reply = (redisReply*)redisCommand(m_redisCtx, "HDEL chat:online_users %s", accountID.c_str());
    if (reply) freeReplyObject(reply);
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
    if (!m_redisCtx || username.empty()) return buildFriendListJson(getFriendListFromDB(username));

    std::string cacheKey = "chat:friends:" + username;

    {
        std::lock_guard<std::mutex> lock(m_redisMutex);
        redisReply* reply = (redisReply*)redisCommand(m_redisCtx, "GET %s", cacheKey.c_str());
        if (reply) {
            if (reply->type == REDIS_REPLY_STRING) {
                std::string cached = reply->str;
                freeReplyObject(reply);
                return cached;
            }
            freeReplyObject(reply);
        }
    }

    std::vector<std::string> friends = getFriendListFromDB(username);
    std::string jsonResponse = buildFriendListJson(friends);

    {
        std::lock_guard<std::mutex> lock(m_redisMutex);
        redisReply* reply = (redisReply*)redisCommand(m_redisCtx, "SETEX %s %d %s", cacheKey.c_str(), 300, jsonResponse.c_str());
        if (reply) freeReplyObject(reply);
    }

    return jsonResponse;
}

void EpollChatServer::invalidateFriendListCache(const std::string& username) {
    if (!m_redisCtx || username.empty()) return;
    std::string cacheKey = "chat:friends:" + username;

    std::lock_guard<std::mutex> lock(m_redisMutex);
    redisReply* reply = (redisReply*)redisCommand(m_redisCtx, "DEL %s", cacheKey.c_str());
    if (reply) freeReplyObject(reply);
}

void EpollChatServer::pushOfflineMessage(const std::string& targetUser, const std::string& messageJson) {
    if (!m_redisCtx || targetUser.empty() || messageJson.empty()) return;

    std::string key = "chat:offline:" + targetUser;
    std::lock_guard<std::mutex> lock(m_redisMutex);
    redisReply* pushReply = (redisReply*)redisCommand(m_redisCtx, "RPUSH %s %s", key.c_str(), messageJson.c_str());
    if (pushReply) freeReplyObject(pushReply);

    redisReply* expireReply = (redisReply*)redisCommand(m_redisCtx, "EXPIRE %s %d", key.c_str(), 604800);
    if (expireReply) freeReplyObject(expireReply);
}

std::vector<std::string> EpollChatServer::popOfflineMessages(const std::string& username) {
    std::vector<std::string> messages;
    if (!m_redisCtx || username.empty()) return messages;

    std::string key = "chat:offline:" + username;
    std::lock_guard<std::mutex> lock(m_redisMutex);

    redisReply* rangeReply = (redisReply*)redisCommand(m_redisCtx, "LRANGE %s 0 -1", key.c_str());
    if (rangeReply) {
        if (rangeReply->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < rangeReply->elements; ++i) {
                redisReply* item = rangeReply->element[i];
                if (item && item->type == REDIS_REPLY_STRING) {
                    messages.emplace_back(item->str);
                }
            }
        }
        freeReplyObject(rangeReply);
    }

    redisReply* delReply = (redisReply*)redisCommand(m_redisCtx, "DEL %s", key.c_str());
    if (delReply) freeReplyObject(delReply);

    return messages;
}

void EpollChatServer::saveMessageToDB(const std::string& sender, const std::string& target, const std::string& content) {
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    char* escapedContent = new char[content.length() * 2 + 1];
    mysql_real_escape_string(m_mysql, escapedContent, content.c_str(), content.length());
    std::string sql = "INSERT INTO all_messages_log (sender, target, content) VALUES ('" + sender + "', '" + target + "', '" + escapedContent + "')";
    if (mysql_query(m_mysql, sql.c_str())) {
        // log("SQL错误: " + std::string(mysql_error(m_mysql)));
    }
    delete[] escapedContent;
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
    std::string sql = "SELECT 1 FROM accounts WHERE username = '" + username + "' LIMIT 1";
    if (mysql_query(m_mysql, sql.c_str())) return false;
    MYSQL_RES* res = mysql_store_result(m_mysql);
    bool exists = (res && mysql_num_rows(res) > 0);
    mysql_free_result(res);
    return exists;
}

bool EpollChatServer::addFriendToDB(const std::string& user, const std::string& friendName) {
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    std::string sql = "INSERT IGNORE INTO friends (user_name, friend_name) VALUES ('" + user + "', '" + friendName + "')";
    mysql_query(m_mysql, sql.c_str());
    std::string sqlReverse = "INSERT IGNORE INTO friends (user_name, friend_name) VALUES ('" + friendName + "', '" + user + "')";
    mysql_query(m_mysql, sqlReverse.c_str());
    return mysql_affected_rows(m_mysql) > 0;
}

std::vector<std::string> EpollChatServer::getFriendListFromDB(const std::string& username) {
    auto conn_ptr = DBConnectionPool::getInstance().getConnection();
    MYSQL* m_mysql = conn_ptr.get();
    std::vector<std::string> friends;
    std::string sql = "SELECT friend_name FROM friends WHERE user_name = '" + username + "'";
    if (mysql_query(m_mysql, sql.c_str())) return friends;
    MYSQL_RES* res = mysql_store_result(m_mysql);
    if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) friends.push_back(row[0]);
        mysql_free_result(res);
    }
    return friends;
}