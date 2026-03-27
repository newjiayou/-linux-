cmake_minimum_required(VERSION 3.10)
project(EpollChatServer CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall")

# --- 【新增：寻找并配置 MySQL 库】 ---
# 1. 寻找 mysql 库
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL mysqlclient REQUIRED)

# 2. 添加头文件路径（告诉编译器去哪里找 mysql.h）
include_directories(${MYSQL_INCLUDE_DIRS})

# 添加可执行文件
add_executable(chatserver 
    main.cpp 
    EpollChatServer.cpp
    ThreadPool.cpp
)

# --- 【新增：链接 MySQL 库】 ---
# 3. 告诉链接器，把 mysql 库链接到你的程序中
target_link_libraries(chatserver ${MYSQL_LIBRARIES} pthread)
