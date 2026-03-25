#include "EpollChatServer.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_EVENTS 1024
#define READ_BUFFER_SIZE 4096

// 构造函数，初始化线程池（假设开4个工作线程）
EpollChatServer::EpollChatServer(uint16_t port) 
    : m_port(port), m_listenFd(-1), m_epollFd(-1), m_threadPool(4), m_mysql(nullptr) {}
EpollChatServer::~EpollChatServer() {
    if (m_listenFd != -1) close(m_listenFd);
    if (m_epollFd != -1) close(m_epollFd);
    // shared_ptr 会自动清理内存，这里只需要关闭 fd
    std::lock_guard<std::mutex> lock(m_mapMutex);
    for (auto& pair : m_clients) {
        close(pair.first);
    }
}

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
    if (!initDB()) return false; // 初始化数据库
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

    m_epollFd = epoll_create1(0);
    if (m_epollFd < 0) return false;

    struct epoll_event event{};
    event.data.fd = m_listenFd;
    event.events = EPOLLIN; 
    epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_listenFd, &event);

    log("服务器启动，监听端口: " + std::to_string(m_port));
    run();
    return true;
}

void EpollChatServer::run() {
    struct epoll_event events[MAX_EVENTS];
    while (true) {
        int numEvents = epoll_wait(m_epollFd, events, MAX_EVENTS, -1);
        for (int i = 0; i < numEvents; i++) {
            int fd = events[i].data.fd;
            if (fd == m_listenFd) {
                handleAccept();
            } else if (events[i].events & EPOLLIN) {
                handleRead(fd);
            } else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                handleDisconnect(fd);
            }
        }
    }
}

void EpollChatServer::handleAccept() {
    struct sockaddr_in clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    int clientFd = accept(m_listenFd, (struct sockaddr*)&clientAddr, &clientLen);
    
    if (clientFd >= 0) {
        setNonBlocking(clientFd);
        struct epoll_event event{};
        event.data.fd = clientFd;
        event.events = EPOLLIN;
        epoll_ctl(m_epollFd, EPOLL_CTL_ADD, clientFd, &event);

        auto ctx = std::make_shared<ClientContext>();
        ctx->fd = clientFd;
        ctx->ip = inet_ntoa(clientAddr.sin_addr);

        std::lock_guard<std::mutex> lock(m_mapMutex);
        m_clients[clientFd] = ctx;

        log("新物理连接: " + ctx->ip + " (fd: " + std::to_string(clientFd) + ")");
        log("当前在线连接数: " + std::to_string(m_clients.size()));
    }
}

void EpollChatServer::handleRead(int fd) {
    char buf[READ_BUFFER_SIZE];
    int bytesRead = recv(fd, buf, sizeof(buf), 0);

    if (bytesRead <= 0) {
        if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        handleDisconnect(fd);
        return;
    }

    std::shared_ptr<ClientContext> ctx;
    {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        if (m_clients.count(fd)) ctx = m_clients[fd];
    }
    
    if (!ctx) return;

    // 保护 buffer 操作
    {
        std::lock_guard<std::mutex> lock(ctx->clientMutex);
        ctx->buffer.insert(ctx->buffer.end(), buf, buf + bytesRead);

        while (ctx->buffer.size() >= sizeof(uint32_t)) {
            uint32_t totalLength;
            memcpy(&totalLength, ctx->buffer.data(), sizeof(uint32_t));
            totalLength = ntohl(totalLength);

            if (ctx->buffer.size() < totalLength) break;

            uint16_t msgType;
            memcpy(&msgType, ctx->buffer.data() + 4, sizeof(uint16_t));
            msgType = ntohs(msgType); 

            std::string body((char*)ctx->buffer.data() + 6, totalLength - 6);

            // 将业务逻辑丢入线程池，ctx 引用计数+1，保证在 Worker 运行时内存不被销毁
            m_threadPool.enqueue([this, ctx, msgType, body]() {
                this->processPacket(ctx, msgType, body);
            });

            ctx->buffer.erase(ctx->buffer.begin(), ctx->buffer.begin() + totalLength);
        }
    }
}

void EpollChatServer::processPacket(std::shared_ptr<ClientContext> ctx, uint16_t msgType, const std::string& body) {
    int clientFd = ctx->fd;

    if (msgType == 3) {

        
        std::string senderID = extractJsonValue(body, "sender");
        if (!senderID.empty()) {
            std::lock_guard<std::mutex> lock(m_mapMutex);
            m_userMap[senderID] = clientFd;
            ctx->accountID = senderID;
            log("身份识别 [类型3]: 账号 " + senderID + " 已绑定");
        }
    }
    else if (msgType == 1) {
        std::string senderID = extractJsonValue(body, "sender");
        std::string target = extractJsonValue(body, "target");
        std::string content = extractJsonValue(body, "content"); 
        log(senderID+target+content) ;
        saveMessageToDB(senderID, target, content);
        if (target == "broadcast") {
            std::vector<int> targetFds;
            {
                std::lock_guard<std::mutex> lock(m_mapMutex);
                for (const auto& pair : m_clients) {
                    if (pair.first != clientFd) targetFds.push_back(pair.first);
                }
            }
            // 在锁外发送，防止 sendPacket 再次请求 m_mapMutex 导致死锁
            for (int tFd : targetFds) sendPacket(tFd, 1, body);
        } else {
            int targetFd = -1;
            {
                std::lock_guard<std::mutex> lock(m_mapMutex);
                if (m_userMap.count(target)) targetFd = m_userMap[target];
            }
            
            if (targetFd != -1) {
                sendPacket(targetFd, 1, body);
                log("私聊: " + senderID + " -> " + target);
            } else {
                log("转发失败: 目标 " + target + " 不在线");
            }
        }
    }
    else if (msgType == 2) {
        sendPacket(clientFd, 2, "");
    }
}

void EpollChatServer::sendPacket(int fd, uint16_t type, const std::string& data) {
    // 1. 组包 (局部变量安全)
    uint32_t totalLength = 6 + (uint32_t)data.size();
    uint32_t netLen = htonl(totalLength);
    uint16_t netType = htons(type);

    std::vector<uint8_t> packet;
    packet.resize(totalLength);
    memcpy(packet.data(), &netLen, 4);
    memcpy(packet.data() + 4, &netType, 2);
    if (!data.empty()) memcpy(packet.data() + 6, data.data(), data.size());

    // 2. 获取上下文并锁定发送
    std::shared_ptr<ClientContext> ctx;
    {
        std::lock_guard<std::mutex> mapLock(m_mapMutex);
        auto it = m_clients.find(fd);
        if (it == m_clients.end()) return; 
        ctx = it->second;
    } 

    // 3. 锁定发送过程，确保包原子性
    std::lock_guard<std::mutex> sendLock(ctx->sendMutex); 
    size_t totalSent = 0;
    while (totalSent < packet.size()) {
        int sent = send(fd, packet.data() + totalSent, packet.size() - totalSent, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100); 
                continue;
            }
            break; 
        }
        totalSent += sent;
    }
}

void EpollChatServer::handleDisconnect(int fd) {
    int currentCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        auto it = m_clients.find(fd);
        if (it != m_clients.end()) {
            if (!it->second->accountID.empty()) {
                m_userMap.erase(it->second->accountID);
                log("账号 " + it->second->accountID + " 下线");
            }
            m_clients.erase(it); // shared_ptr 计数减 1
        }
        currentCount = m_clients.size();
    }

    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    log("当前在线连接数: " + std::to_string(currentCount));
}

bool EpollChatServer::initDB() {
    m_mysql = mysql_init(NULL);
    // 参数：句柄, 地址, 用户名, 密码, 数据库名, 端口, unix_socket, 标志
    if (!mysql_real_connect(m_mysql, "192.168.56.101", "root_1", "123456Zxj!", "chat_system", 0, NULL, 0)) {
        log("数据库连接失败: " + std::string(mysql_error(m_mysql)));
        return false;
    }
    log("数据库连接成功");
    return true;
}

void EpollChatServer::saveMessageToDB(const std::string& sender, const std::string& target, const std::string& content) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    
    // 处理特殊字符防止 SQL 注入
    char* escapedContent = new char[content.length() * 2 + 1];
    mysql_real_escape_string(m_mysql, escapedContent, content.c_str(), content.length());

    std::string sql = "INSERT INTO all_messages_log (sender, target, content) VALUES ('" 
                      + sender + "', '" + target + "', '" + escapedContent + "')";
    
    if (mysql_query(m_mysql, sql.c_str())) {
        log("SQL 执行错误: " + std::string(mysql_error(m_mysql)));
    }
    
    delete[] escapedContent;
}