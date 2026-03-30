
# Epoll 高性能聊天服务器

这是一个基于 Linux 原生 epoll I/O 多路复用技术和 C++11 实现的轻量级、高性能聊天服务器。它采用主（I/O）从（Worker）线程模型：主线程负责 epoll 网络事件的监听与分发，并将耗时的业务逻辑（如数据库操作、消息处理）异步投递到后台线程池中，以实现高并发下的低延迟响应和高吞吐量。

## 核心特性

- **高性能网络 IO**：利用 epoll 的水平触发（LT）模式，高效处理海量并发连接。所有 socket 均设为非阻塞模式，避免任何 I/O 操作阻塞主线程。
- **多线程业务处理**：内置一个固定大小的线程池，负责处理所有业务逻辑，实现了网络 I/O 与业务计算的解耦，充分利用多核 CPU 资源。
- **数据库集成**：
    - **MySQL 连接**：内置 MySQL C API 的封装，支持用户认证、好友关系管理、消息持久化等功能。
    - **线程安全**：通过互斥锁（`std::mutex`）保证对共享数据库连接的访问是线程安全的。
    - **防 SQL 注入**：关键的数据库查询（如用户登录、历史消息查询）采用预处理语句（`Prepared Statements`），从根本上防止 SQL 注入攻击。
- **完备的聊天功能**：
    - **用户系统**：支持用户登录验证。
    - **消息系统**：支持一对一私聊、群发广播。
    - **好友系统**：支持好友列表获取、好友添加功能。
    - **历史消息同步**：客户端可请求指定时间戳之后的增量历史消息，实现多端消息同步。
- **自定义应用层协议**：
    - **粘包/半包处理**：采用 `总长度 (4字节) + 消息类型 (2字节) + Body (JSON)` 的二进制协议格式，精确地分割 TCP 数据流，确保消息的完整性。
- **精细化并发控制**：
    - 使用 `std::shared_ptr` 配合 `std::mutex` 管理客户端连接的生命周期与数据访问，确保在多线程环境下操作的原子性和安全性。

## 协议格式

每个数据包都由一个 6 字节的头部和一个可变长度的 JSON 消息体组成。

| 字段 | 长度 (字节) | 数据类型 | 说明 |
| :--- | :--- | :--- |:---|
| `Total Length` | 4 | `uint32_t` | 整个数据包的长度（包含头部的 6 字节），网络字节序。 |
| `Message Type` | 2 | `uint16_t` | 消息的功能类型，网络字节序。 |
| `Body` | `Total Length - 6` | `string` | JSON 格式的业务数据。 |

### 消息类型 (Message Type)

| Type ID | 方向 | 功能描述 | Body (JSON) 示例 |
|:---:|:---:|---|---|
| 1 | C -> S | 发送聊天消息 | `{"sender":"userA","target":"userB","message":"你好"}` 或 `{"target":"broadcast",...}` |
| 1 | S -> C | 转发/回传聊天消息 | `{"sender":"userA","target":"userB","message":"你好","timestamp":"2023-10-27 15:30:05"}` |
| 2 | C <-> S| 心跳包 | *(Body 为空)* |
| 4 | C -> S | 用户登录请求 | `{"username":"userA","password":"123456"}` |
| 5 | S -> C | 登录结果响应 | `{"result":"success"}` 或 `{"result":"fail"}` |
| 7 | C -> S | 请求增量历史消息 | `{"last_timestamp":"2023-10-27 15:00:00"}` |
| 8 | S -> C | 返回增量历史消息 | `[{"sender":...}, {"sender":...}]` (一个 JSON 数组) |
| 9 | C -> S | 添加好友请求 | `{"friend":"userB"}` |
| 10 | S -> C | 添加好友结果 | `{"result":"success","friend":"userB","message":"添加成功"}` 或 `{"result":"fail",...}`|
| 11 | C -> S | 请求好友列表 | *(Body 为空)* |
| 12 | S -> C | 返回好友列表 | `{"friends":["userB","userC"]}` |

## 编译与运行

### 环境要求

*   操作系统：Linux
*   编译器：支持 C++11 或更高版本的 GCC/Clang
*   构建工具：CMake 3.10+
*   数据库：MySQL/MariaDB
*   **依赖库**：MySQL 开发库（通常包名为 `libmysqlclient-dev` 或 `mysql-devel`）

### 编译步骤

```bash
# 1. 确保已安装 g++, cmake 和 mysql 开发库
# 例如, 在 Ubuntu/Debian 上:
# sudo apt-get update
# sudo apt-get install build-essential cmake libmysqlclient-dev

# 2. 克隆项目并创建 build 目录
git clone <your-repo-url>
cd <your-project-directory>
mkdir build
cd build

# 3. 运行 CMake 和 Make
cmake ..
make
```

### 数据库准备

请确保您的 MySQL 数据库中已创建了对应的库和表。根据代码中的 SQL 语句，您需要至少三张表：

1.  **`accounts`** (用户信息表)
    ```sql
    CREATE TABLE accounts (
        username VARCHAR(64) PRIMARY KEY,
        password VARCHAR(64) NOT NULL
    );
    ```
2.  **`all_messages_log`** (消息历史记录表)
    ```sql
    CREATE TABLE all_messages_log (
        id INT AUTO_INCREMENT PRIMARY KEY,
        sender VARCHAR(64),
        target VARCHAR(64),
        content TEXT,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    );
    CREATE INDEX idx_created_at ON all_messages_log(created_at);
    ```
3.  **`friends`** (好友关系表)
    ```sql
    CREATE TABLE friends (
        user_name VARCHAR(64),
        friend_name VARCHAR(64),
        PRIMARY KEY (user_name, friend_name)
    );
    ```

### 运行服务器

```bash
# 在 build 目录下执行
# 启动服务器，使用默认端口 12345
./EpollChatServer

# 或指定一个端口
./EpollChatServer 9999
```

## 核心技术点

-   **网络模型**：`socket` -> `bind` -> `listen` -> `epoll_create` -> `epoll_wait` 事件循环。
-   **非阻塞 I/O**：通过 `fcntl` 将所有文件描述符设置为 `O_NONBLOCK`，配合 epoll 高效处理网络事件。
-   **并发安全**：
    -   `m_mapMutex`: 保护全局的客户端映射 `m_clients` 和用户映射 `m_userMap` 的增删查改。
    -   `m_dbMutex`: 保护对单一 `MYSQL*` 句柄的访问，确保数据库操作的串行化。
    -   `ClientContext::sendMutex`: 保护对单个客户端的 `send` 操作，防止多个线程向同一个 fd 并发写入导致数据包交错。
    -   `ClientContext::clientMutex`: 保护对单个客户端的 `buffer` 的读写，处理粘包时防止竞争。
-   **资源管理**：
    -   使用 `std::shared_ptr<ClientContext>` 管理客户端上下文的生命周期。当一个连接断开或一个异步任务完成时，智能指针可以自动管理其资源的释放。

## 未来改进方向

-   **配置文件**：将数据库连接信息、服务器端口、线程池大小等硬编码的常量移至外部配置文件（如 `JSON` 或 `YAML`）。
-   **日志系统**：引入专业的日志库（如 `spdlog`），实现日志分级、异步写入和文件轮转。
-   **心跳检测**：完善心跳机制，增加一个定时器（如基于 `timerfd_create`），定期检查所有客户端的最后活跃时间，自动踢掉僵尸连接。
-   **ET 模式**：将 epoll 的 LT（水平触发）模式改为 ET（边缘触发）模式，并配合 `while(recv)` 循环一次性读完所有数据，可进一步提升 I/O 效率。
