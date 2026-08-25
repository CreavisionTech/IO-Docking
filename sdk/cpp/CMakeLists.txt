cmake_minimum_required(VERSION 3.10)
project(IODockSDK VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找线程库
find_package(Threads REQUIRED)

# SDK 库
add_library(iodock STATIC
    iodock.cpp
)
target_include_directories(iodock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(iodock PUBLIC Threads::Threads)

# Windows 需要链接串口相关库
if(WIN32)
    target_link_libraries(iodock PRIVATE setupapi)
endif()

# 演示程序
add_executable(demo demo.cpp)
target_link_libraries(demo PRIVATE iodock)

# 安装
install(TARGETS iodock
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(FILES iodock.h DESTINATION include)
install(TARGETS demo RUNTIME DESTINATION bin)
