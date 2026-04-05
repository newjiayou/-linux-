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
#include "DBConnectionPool.h" // 新增：包含连接池头文件
// 客户端连接状态上下文
struct ClientContext {
    int fd;
    std::string ip;
    std::vector<uint8_t> buffer; // 处理粘包的缓冲区
    std::string accountID;
    std::mutex clientMutex;  
    std::mutex sendMutex; 
    // 用户态发送缓冲：当内核 socket 发送缓冲写不进去（EAGAIN）时暂存待发数据
    std::vector<uint8_t> sendBuffer;
    size_t sendOffset = 0;
};

class EpollChatServer; // 前向声明

class SubReactor {
public:
    SubReactor(EpollChatServer* server);
    ~SubReactor();

    // 启动子 Reactor 的事件循环
    void run();
    
    // 向该子 Reactor 的 epoll 实例中添加一个新的文件描述符
    void addFd(int fd);

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
    //多线程
    std::mutex m_mapMutex;
    std::mutex m_sendMutex; // 保护发送操作的原子性
    std::mutex m_redisMutex;

    std::vector<std::unique_ptr<SubReactor>> m_subReactors; // 存储所有从 Reactor
    std::atomic<size_t> m_nextSubReactor{0}; // 用于轮询选择下一个从 Reactor 的索引
    friend class SubReactor;

    ThreadPool m_threadPool;
    bool m_enableDBWrites = true;
    redisContext* m_redisCtx = nullptr;
    //内部辅助函数
   void log(const std::string& msg);
    void setNonBlocking(int fd);
    std::string extractJsonValue(const std::string& json, const std::string& key);
    // Epoll 事件驱动
    void run();
    void handleAccept();
    void handleRead(int fd);
    void handleWrite(int fd);
    void handleDisconnect(int fd);
    // 业务逻辑与发包机制
    void processPacket(std::shared_ptr<ClientContext> ctx, uint16_t msgType, const std::string& body);
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
    void setOnlineUser(const std::string& accountID, int fd);
    int getOnlineFd(const std::string& accountID);
    void removeOnlineUser(const std::string& accountID);

    std::string buildFriendListJson(const std::vector<std::string>& friends);
    std::string getFriendListJson(const std::string& username);
    void invalidateFriendListCache(const std::string& username);

    void pushOfflineMessage(const std::string& targetUser, const std::string& messageJson);
    std::vector<std::string> popOfflineMessages(const std::string& username);
};
#endif // EPOLL_CHAT_SERVER_H
