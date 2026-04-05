#include "EpollChatServer.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    uint16_t port = 12345;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    std::cout << "--- 高性能 epoll 聊天服务端启动 ---" << std::endl;
    EpollChatServer server(port);
    
    if (!server.start()) {
        std::cerr << "服务器启动失败！请检查端口是否被占用。" << std::endl;
        return -1;
    }

    return 0;
}