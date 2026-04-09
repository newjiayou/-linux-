# linux_server

基于 `epoll + 多线程 + MySQL + Redis` 的高并发聊天服务端（C++17）。

项目核心是一个 Linux 下的 TCP 长连接聊天服务器：
- 主 Reactor 负责监听并 `accept` 新连接
- 多个 Sub-Reactor（按 CPU 核心数创建）负责 I/O 读写事件
- 业务逻辑投递到线程池异步执行
- MySQL 做持久化（账号、好友、聊天记录）
- Redis 做缓存与离线消息队列
- 自研内存池（`v2/`）用于收发缓冲分配，减少频繁 `malloc/free`

---

## 1. 功能概览

- 私聊 / 广播消息
- 登录鉴权
- 好友添加与好友列表查询
- 历史消息同步
- 离线消息暂存与上线拉取（Redis）
- 连接池：MySQL 连接池 + Redis 连接池
- 发送背压保护（发送缓冲过大时断开连接）

---

## 2. 协议说明（当前实现）

每个包格式：

| 字段 | 长度 | 说明 |
|---|---:|---|
| Length | 4 字节 | 包总长度（含头部）|
| Type | 2 字节 | 消息类型 |
| Body | 变长 | JSON 字符串 |

> `Length` 为网络字节序（`htonl/ntohl`），`Type` 为网络字节序（`htons/ntohs`）。

### 已实现消息类型

- `1`：聊天消息（私聊/广播）
- `2`：心跳
- `3`：身份绑定（sender -> 当前连接）
- `4`：登录请求（username/password）
- `5`：登录结果
- `7`：历史消息同步请求
- `8`：历史消息同步响应
- `9`：添加好友请求
- `10`：添加好友结果
- `11`：好友列表请求
- `12`：好友列表响应

---

## 3. 目录结构

```text
.
├─ main.cpp
├─ EpollChatServer.h / EpollChatServer.cpp   # 服务器核心（Reactor、协议、业务）
├─ ThreadPool.h / ThreadPool.cpp             # 线程池
├─ DBConnectionPool.h / DBConnectionPool.cpp # MySQL 连接池
├─ RedisConnectionPool.h / RedisConnectionPool.cpp # Redis 连接池
├─ v2/                                        # 自研内存池
│  ├─ include/
│  ├─ src/
│  └─ tests/
├─ scripts/                                   # 压测、监控、运维脚本
└─ CMakeLists.txt
```

---

## 4. 构建与运行

## 依赖

- Linux
- CMake >= 3.10
- C++17 编译器（GCC/Clang）
- `pthread`
- `pkg-config`
- `libmysqlclient`（MySQL C API）
- `hiredis`

Debian/Ubuntu 示例：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libmysqlclient-dev libhiredis-dev
```

## 编译

```bash
mkdir -p build
cd build
cmake ..
make -j
```

产物：`chatserver`

## 运行

```bash
./chatserver              # 默认 12345 端口
# 或
./chatserver 23456        # 指定端口
```

---

## 5. 数据源与配置

## MySQL

当前代码中 MySQL 参数在 `EpollChatServer::initDB()` 内写死：
- host: `192.168.56.101`
- user: `root_1`
- password: `123456Zxj!`
- db: `chat_system`
- pool size: `20`

如果你的环境不同，请先修改该函数再编译。

## Redis

Redis 支持环境变量（未设置时走默认值）：
- `REDIS_HOST`（默认 `127.0.0.1`）
- `REDIS_PORT`（默认 `6379`）
- `REDIS_POOL_SIZE`（默认 `16`）

示例：

```bash
export REDIS_HOST=127.0.0.1
export REDIS_PORT=6379
export REDIS_POOL_SIZE=32
./chatserver
```

---

## 6. 关键实现要点

- `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)`：新连接直接非阻塞
- `EPOLLET` 边缘触发 + 循环 `recv` 到 `EAGAIN`
- 每连接独立接收/发送缓冲（基于内存池）
- 发送路径支持用户态积压 + `EPOLLOUT` 续写
- 发送积压上限（默认 4MB）防止慢连接拖垮服务
- 统计计数器周期打印（accept/read/login/send 等）方便压测观测

---

## 7. scripts 脚本说明

`scripts/` 下包含压测与辅助脚本，例如：
- `bench_chat_latency.py` / `bench_chat.sh`：聊天时延压测
- `bench_tcpkali_throughput.sh`：吞吐压测
- `monitor.sh`：运行监控
- `log_rotate.sh`：日志轮转

> 请根据你的部署路径与环境自行调整脚本中的参数。

---

## 8. v2 内存池

`v2/` 是独立的内存池模块，提供单测与性能测试：

```bash
cd v2
mkdir -p build
cd build
cmake ..
make -j
./unit_test
./perf_test
```

---

## 9. 已知注意事项

- 目前 JSON 解析为手写字符串提取（非完整 JSON 解析器）
- 部分 SQL 语句仍为字符串拼接，生产环境建议统一改为预处理语句
- `SubReactor` 线程退出流程较简化，生产环境建议增加优雅停机机制

---

## 10. 启动信息

服务启动后会输出类似：

```text
--- 高性能 epoll 聊天服务端启动 ---
[LOG] 启动 N 个 I/O 线程 (Sub-Reactors)
[STATS] accept=... read=... login_in=... login_db=... login_ok=... login_fail=... send_calls=... backpressure_drop=...
```
