cmake_minimum_required(VERSION 3.10)
project(EpollChatServer CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall")

find_package(PkgConfig REQUIRED)

# MySQL
pkg_check_modules(MYSQLCLIENT REQUIRED mysqlclient)

# Redis (hiredis)
pkg_check_modules(HIREDIS REQUIRED hiredis)

# 头文件路径
include_directories(${MYSQLCLIENT_INCLUDE_DIRS})
include_directories(${HIREDIS_INCLUDE_DIRS})

# 添加可执行文件
add_executable(chatserver
    main.cpp
    EpollChatServer.cpp
    ThreadPool.cpp
    DBConnectionPool.cpp
)

target_link_libraries(chatserver PRIVATE pthread ${MYSQLCLIENT_LIBRARIES} ${HIREDIS_LIBRARIES})
target_include_directories(chatserver PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS} ${HIREDIS_INCLUDE_DIRS})