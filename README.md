EpollChatServer
这是一个基于 Linux 原生 epoll I/O 多路复用技术实现的轻量级、高性能聊天服务器。它采用了主从反应堆（Reactor）思想的变体：使用单线程 epoll 处理网络事件，并将具体的业务逻辑处理分发到后台线程池，以实现高并发下的低延迟响应。
核心特性
高性能 IO：利用 epoll 的边缘触发（ET）或水平触发（LT）特性（当前代码为 LT），能够高效处理大量并发连接。
线程池处理：将业务逻辑（如 JSON 解析、消息转发、身份绑定）卸载到线程池中，避免阻塞网络主线程。
粘包处理：自定义二进制协议头（长度 + 类型），确保 TCP 流式传输中的数据包完整性。
线程安全：通过 std::mutex 对客户端上下文（ClientContext）和全局连接映射进行精细化保护，防止多线程竞争导致的数据竞争。
多端通信：支持通过 accountID 进行私聊，以及简单的广播模式。
协议格式
每个数据包结构如下：
| 字段 | 长度 | 说明 |
| :--- | :--- | :--- |
| Length | 4 字节 | 包总长度（包含头部 6 字节） |
| Type | 2 字节 | 消息类型 (1: 聊天, 2: 心跳, 3: 鉴权) |
| Body | 变长 | JSON 格式数据 |
目录结构
main.cpp: 程序入口。
EpollChatServer.h/cpp: 服务器核心逻辑，负责 epoll 事件循环、连接管理及业务分发。
ThreadPool.h/cpp: 基于条件变量和互斥锁实现的任务队列线程池。
编译指南
环境要求
Linux 系统
支持 C++11 的编译器 (如 GCC/Clang)
CMake 3.10+
编译命令
code
Bash
# 在项目根目录下执行
mkdir build
cd build
cmake ..
make
运行
code
Bash
# 启动服务器 (默认端口 12345)
./chatserver [端口号]
核心技术点
网络模型：socket -> bind -> listen -> epoll_create -> epoll_wait 事件循环。
非阻塞 IO：通过 fcntl 将文件描述符设置为 O_NONBLOCK，配合 epoll 处理网络中断。
并发安全：
m_mapMutex：保护全局 m_clients 和 m_userMap 的增删查改。
clientMutex：保护单个客户端的读写缓存，防止分包处理与发送逻辑冲突。
sendMutex：确保 send 操作的完整性，防止多个线程同时向同一个 fd 发送导致数据错乱。
TODO / 改进建议

边缘触发优化：当前使用 LT 模式，可改为 ET 模式并结合 while(recv) 循环处理。

零拷贝优化：在发送数据时，当前采用了 std::vector 复制，未来可考虑使用 sendfile 或缓冲区链表。

心跳检测：引入 Timer 机制，自动清理长时间无心跳（msgType=2）的连接。

更完善的协议：引入 protobuf 或其他序列化库替代原始的字符串解析，提升性能。
贡献
欢迎提交 Issue 或 Pull Request，帮助改进该服务器的性能与稳定性。
