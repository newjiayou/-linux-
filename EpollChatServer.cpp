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
    std::string content = extractJsonValue(body, "message"); // 修正：对应你客户端发的 key 是 message

    // 1. 获取服务器当前的权威时间
    std::string serverTime = getServerTimeStr(); // 参考之前给你的生成时间字符串的函数

    // 2. 存入数据库
    saveMessageToDB(senderID, target, content);

    // 3. 【核心步骤】重新构造 JSON 报文，把服务器时间戳塞进去
    // 这样所有人（包括你自己）收到的包里都有这个权威时间
    std::string enrichedBody = "{\"sender\":\"" + senderID + 
                               "\",\"target\":\"" + target + 
                               "\",\"message\":\"" + content + 
                               "\",\"timestamp\":\"" + serverTime + "\"}";

    if (target == "broadcast") {
        std::vector<int> targetFds;
        {
            std::lock_guard<std::mutex> lock(m_mapMutex);
            for (const auto& pair : m_clients) {
                // 【修改点】：不再排除 clientFd，发给所有人
                targetFds.push_back(pair.first);
            }
        }
        for (int tFd : targetFds) sendPacket(tFd, 1, enrichedBody);
    } else {
        int targetFd = -1;
        {
            std::lock_guard<std::mutex> lock(m_mapMutex);
            if (m_userMap.count(target)) targetFd = m_userMap[target];
        }
        
        if (targetFd != -1) {
            // 【私聊逻辑修改】：
            // 第一份：发给接收者
            sendPacket(targetFd, 1, enrichedBody);
            // 第二份：回传给发送者（你自己）
            sendPacket(clientFd, 1, enrichedBody); 
            
            log("私聊转发及回传完成: " + senderID + " -> " + target);
        } else {
            // 如果目标不在线，也要给发送者回一个失败的提示（可选）
            log("转发失败: 目标 " + target + " 不在线");
        }
    }
}
    else if (msgType == 2) {
        sendPacket(clientFd, 2, "");
    }
    else if (msgType == 4)
    {
        std::string username = extractJsonValue(body, "username");
        std::string password = extractJsonValue(body, "password");
        if (checkLoginFromDatabase(username, password)) {
            log("用户 " + username + " 登录成功");
            
            // 绑定身份 (这就是以前 Type 3 做的事)
            {
                std::lock_guard<std::mutex> lock(m_mapMutex);
                m_userMap[username] = clientFd;
                ctx->accountID = username;
            }
            
            sendPacket(clientFd, 5, "{\"result\":\"success\"}");
            std::vector<std::string> friends = getFriendListFromDB(username);
    
    // 构造 JSON: {"friends":["张三","李四"]}
    std::string jsonResponse = "{\"friends\":[";
    for (size_t i = 0; i < friends.size(); ++i) {
        jsonResponse += "\"" + friends[i] + "\"";
        if (i < friends.size() - 1) jsonResponse += ",";
    }
    jsonResponse += "]}";

    sendPacket(clientFd, 12, jsonResponse);
            
        } else {
            log("用户 " + username + " 登录失败：凭据错误");
            sendPacket(clientFd, 5, "{\"result\":\"fail\"}");
        }
    }
    else if (msgType == 7) // 1. 识别包类型为 7（历史记录同步请求）
{
    // 从 JSON 中提取客户端本地最后一条消息的时间（即客户端已拥有的最新记录的时间）
    std::string lastTime = extractJsonValue(body, "last_timestamp");

    // 从 ctx 获取当前连接的账号 ID
    std::string currentUser = ctx->accountID; 

    if (currentUser.empty()) {
        log("错误：未登录用户尝试请求历史记录");
        return;
    }

    log("用户 [" + currentUser + "] 请求 [" + lastTime + "] 之后的增量消息");

    // --- 数据库操作区 ---

    std::lock_guard<std::mutex> lock(m_dbMutex);

    MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);

    // 【修正点】：字段名改为 created_at，并增加 sender = ? 的逻辑（如果你想同步自己在其他设备发的消息）
    // 逻辑：时间 > 客户端最后时间 AND (目标是广播 OR 目标是我 OR 发送者是我)
    const char* sql = "SELECT sender, target, content, created_at FROM all_messages_log "
                      "WHERE created_at > ? AND (target = 'broadcast' OR target = ? OR sender = ?) "
                      "ORDER BY created_at ASC";

    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        log("预处理失败: " + std::string(mysql_stmt_error(stmt)));
        mysql_stmt_close(stmt);
        return;
    }

    // --- 绑定输入参数 (填充 SQL 里的三个问号) ---
    MYSQL_BIND bind_in[3]; // 增加到 3 个参数
    memset(bind_in, 0, sizeof(bind_in));

    // 第一个问号：lastTime (created_at > ?)
    bind_in[0].buffer_type = MYSQL_TYPE_STRING;
    bind_in[0].buffer = (char*)lastTime.c_str();
    bind_in[0].buffer_length = lastTime.length();

    // 第二个问号：目标是我自己 (target = ?)
    bind_in[1].buffer_type = MYSQL_TYPE_STRING;
    bind_in[1].buffer = (char*)currentUser.c_str();
    bind_in[1].buffer_length = currentUser.length();

    // 第三个问号：发送者是我自己 (sender = ?)
    bind_in[2].buffer_type = MYSQL_TYPE_STRING;
    bind_in[2].buffer = (char*)currentUser.c_str();
    bind_in[2].buffer_length = currentUser.length();

    mysql_stmt_bind_param(stmt, bind_in);

    if (mysql_stmt_execute(stmt)) {
        log("查询历史执行失败: " + std::string(mysql_stmt_error(stmt)));
        mysql_stmt_close(stmt);
        return;
    }

    // --- 绑定输出结果 (对应 SELECT 的 4 个字段) ---
    char s_buf[64], t_buf[64], c_buf[1024], ts_buf[64];
    unsigned long s_len, t_len, c_len, ts_len;
    MYSQL_BIND bind_out[4];
    memset(bind_out, 0, sizeof(bind_out));

    // 绑定顺序：0:sender, 1:target, 2:content, 3:created_at
    bind_out[0].buffer_type = MYSQL_TYPE_STRING; bind_out[0].buffer = s_buf; bind_out[0].buffer_length = sizeof(s_buf); bind_out[0].length = &s_len;
    bind_out[1].buffer_type = MYSQL_TYPE_STRING; bind_out[1].buffer = t_buf; bind_out[1].buffer_length = sizeof(t_buf); bind_out[1].length = &t_len;
    bind_out[2].buffer_type = MYSQL_TYPE_STRING; bind_out[2].buffer = c_buf; bind_out[2].buffer_length = sizeof(c_buf); bind_out[2].length = &c_len;
    bind_out[3].buffer_type = MYSQL_TYPE_STRING; bind_out[3].buffer = ts_buf; bind_out[3].buffer_length = sizeof(ts_buf); bind_out[3].length = &ts_len;

    mysql_stmt_bind_result(stmt, bind_out);
    mysql_stmt_store_result(stmt);

    // --- 构造响应 JSON 数组 ---
    std::string jsonResponse = "[";
    bool first = true;

    while (mysql_stmt_fetch(stmt) == 0) {
        if (!first) jsonResponse += ",";
        jsonResponse += "{";
        jsonResponse += "\"sender\":\"" + std::string(s_buf, s_len) + "\",";
        jsonResponse += "\"target\":\"" + std::string(t_buf, t_len) + "\",";
        jsonResponse += "\"content\":\"" + std::string(c_buf, c_len) + "\","; 
        jsonResponse += "\"timestamp\":\"" + std::string(ts_buf, ts_len) + "\""; // JSON 里依然建议用 timestamp 方便客户端
        jsonResponse += "}";
        first = false;
    }
    jsonResponse += "]";

    mysql_stmt_close(stmt);
    log(jsonResponse);
    // 将结果回传给客户端
    sendPacket(ctx->fd, 8, jsonResponse);
    log("增量历史发送完毕，共计消息已打包。");
}
   else if (msgType == 9) {
    std::string targetFriend = extractJsonValue(body, "friend");
    std::string currentUser = ctx->accountID;

    if (currentUser.empty()) return;

    // 1. 不能添加自己
    if (targetFriend == currentUser) {
        sendPacket(clientFd, 10, "{\"result\":\"fail\",\"message\":\"不能添加自己为好友\"}");
    }
    // 2. 检查目标用户是否存在于 accounts 表中
    else if (!userExistsInDB(targetFriend)) {
        sendPacket(clientFd, 10, "{\"result\":\"fail\",\"message\":\"该用户不存在\"}");
    }
    // 3. 执行添加逻辑
    else {
        if (addFriendToDB(currentUser, targetFriend)) {
            log("用户 " + currentUser + " 添加了好友 " + targetFriend);
            sendPacket(clientFd, 10, "{\"result\":\"success\",\"friend\":\"" + targetFriend + "\",\"message\":\"添加成功\"}");
            
            // 可选：如果对方在线，实时通知对方（被添加了）
            // handleFriendAddedNotify(targetFriend, currentUser); 
        } else {
            sendPacket(clientFd, 10, "{\"result\":\"fail\",\"message\":\"已经是好友或系统错误\"}");
        }
    }
}

// --- [新增] 处理获取好友列表请求 ---
else if (msgType == 11) {
    std::string currentUser = ctx->accountID;
    if (currentUser.empty()) return;

    std::vector<std::string> friends = getFriendListFromDB(currentUser);
    
    // 构造 JSON: {"friends":["张三","李四"]}
    std::string jsonResponse = "{\"friends\":[";
    for (size_t i = 0; i < friends.size(); ++i) {
        jsonResponse += "\"" + friends[i] + "\"";
        if (i < friends.size() - 1) jsonResponse += ",";
    }
    jsonResponse += "]}";

    sendPacket(clientFd, 12, jsonResponse);
    log("已向用户 " + currentUser + " 发送好友列表");
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

std::string EpollChatServer:: getServerTimeStr() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    // 格式化为 "2023-10-27 15:30:05" 这种 MySQL/SQLite 通用格式
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

bool EpollChatServer::checkLoginFromDatabase(const std::string& inputUser, const std::string& inputPass) {
    // 1. 加锁保护共享的 m_mysql 句柄
    std::lock_guard<std::mutex> lock(m_dbMutex);

    if (!m_mysql) {
        log("数据库句柄未初始化");
        return false;
    }

    // 2. 初始化预处理语句句柄
    MYSQL_STMT *stmt = mysql_stmt_init(m_mysql);
    if (!stmt) {
        log("预处理初始化失败: " + std::string(mysql_error(m_mysql)));
        return false;
    }

    // 3. 准备 SQL 模板 (请确保表名和字段名与你数据库一致)
    const char* sql = "SELECT password FROM accounts WHERE username = ?";
    if (mysql_stmt_prepare(stmt, sql, strlen(sql))) {
        log("预处理准备失败: " + std::string(mysql_stmt_error(stmt)));
        mysql_stmt_close(stmt);
        return false;
    }

    // 4. 绑定输入参数
    MYSQL_BIND bind_input[1];
    memset(bind_input, 0, sizeof(bind_input));
    bind_input[0].buffer_type = MYSQL_TYPE_STRING;
    bind_input[0].buffer = (char*)inputUser.c_str();
    bind_input[0].buffer_length = inputUser.length();

    if (mysql_stmt_bind_param(stmt, bind_input)) {
        log("参数绑定失败: " + std::string(mysql_stmt_error(stmt)));
        mysql_stmt_close(stmt);
        return false;
    }

    // 5. 执行查询
    if (mysql_stmt_execute(stmt)) {
        log("SQL 执行失败: " + std::string(mysql_stmt_error(stmt)));
        mysql_stmt_close(stmt);
        return false;
    }

    // 6. 绑定输出结果
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

    if (mysql_stmt_bind_result(stmt, bind_output)) {
        log("结果绑定失败: " + std::string(mysql_stmt_error(stmt)));
        mysql_stmt_close(stmt);
        return false;
    }

    // 7. 获取并对比数据
    bool authSuccess = false;
    if (mysql_stmt_fetch(stmt) == 0) { // 找到了该用户
        std::string dbPassStr(db_password, length);
        if (dbPassStr == inputPass) {
            authSuccess = true;
        }
    }

    // 8. 【关键】只关闭 stmt，不要关闭 m_mysql！
    mysql_stmt_close(stmt);

    return authSuccess;
}


bool EpollChatServer::userExistsInDB(const std::string& username) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    std::string sql = "SELECT 1 FROM accounts WHERE username = '" + username + "' LIMIT 1";
    if (mysql_query(m_mysql, sql.c_str())) return false;
    MYSQL_RES* res = mysql_store_result(m_mysql);
    bool exists = (res && mysql_num_rows(res) > 0);
    mysql_free_result(res);
    return exists;
}
bool EpollChatServer::addFriendToDB(const std::string& user, const std::string& friendName) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    // 使用 IGNORE 防止重复插入报错
    std::string sql = "INSERT IGNORE INTO friends (user_name, friend_name) VALUES ('" 
                      + user + "', '" + friendName + "')";
    
    if (mysql_query(m_mysql, sql.c_str())) {
        log("添加好友失败: " + std::string(mysql_error(m_mysql)));
        return false;
    }
    
    // 如果是双向好友，可以再插一条反向的
    std::string sqlReverse = "INSERT IGNORE INTO friends (user_name, friend_name) VALUES ('" 
                             + friendName + "', '" + user + "')";
    mysql_query(m_mysql, sqlReverse.c_str());

    return mysql_affected_rows(m_mysql) > 0;
}
std::vector<std::string> EpollChatServer::getFriendListFromDB(const std::string& username) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    std::vector<std::string> friends;
    
    std::string sql = "SELECT friend_name FROM friends WHERE user_name = '" + username + "'";
    if (mysql_query(m_mysql, sql.c_str())) return friends;

    MYSQL_RES* res = mysql_store_result(m_mysql);
    if (res) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            friends.push_back(row[0]);
        }
        mysql_free_result(res);
    }
    return friends;
}