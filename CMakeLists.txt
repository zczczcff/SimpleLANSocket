cmake_minimum_required(VERSION 3.15)
project(SimpleLANSocket
    VERSION 1.0.0
    LANGUAGES CXX
)

# 收集所有源文件
file(GLOB_RECURSE LIB_SOURCES
    ./*.cpp
)

# 收集所有头文件
file(GLOB_RECURSE LIB_HEADERS
    ./*.h      # 匹配 .h 文件
)

add_library(SimpleLANSocket STATIC ${LIB_SOURCES} ${LIB_HEADERS})


# # 自动收集所有头文件 (递归搜索)
# file(GLOB_RECURSE HEADER_FILES 
#     LIST_DIRECTORIES false
#     RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/Include"
#     "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h"
# #    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
# #    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hxx"
# )

# # 1. 创建 INTERFACE 库目标
# add_library(SimpleLANSocket INTERFACE)

# 2. 添加公共头文件目录（消费者需要包含的路径）
target_include_directories(SimpleLANSocket PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)