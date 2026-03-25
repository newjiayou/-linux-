cmake_minimum_required(VERSION 3.10)
project(EpollChatServer CXX)

# 设置 C++11 或更高标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 开启高等级优化
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall")

# 添加可执行文件
add_executable(chatserver 
    main.cpp 
    EpollChatServer.cpp
    ThreadPool.cpp   # <--- 把这一行加进去！
)