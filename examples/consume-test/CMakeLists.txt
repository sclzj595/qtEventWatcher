# ============================================================
# consume-test - V4 C2 双模型验收载体（独立 CMake 工程）
#
# 注意：不挂进主构建树的 examples/CMakeLists.txt——add_subdirectory
# 模式会指回仓库根，挂载即递归。两种模式均从外部独立 configure。
#
# 模式 1 find_package：
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="<install-prefix>;<qt-root>"
# 模式 2 add_subdirectory：
#   cmake -S . -B build -DQEW_ROOT=<repo-root> -DCMAKE_PREFIX_PATH=<qt-root>
# ============================================================

cmake_minimum_required(VERSION 3.16)
project(ConsumeTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if (QEW_ROOT)
    # 模式 2：源码树直接接入（关闭 tests/examples 防二次拉入）
    set(QT_EVENT_WATCHER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(QT_EVENT_WATCHER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(${QEW_ROOT} QtEventWatcher-build)
    add_library(QtEventWatcher::QtEventWatcherCore ALIAS QtEventWatcherCore)
else ()
    # 模式 1：安装树接入
    find_package(QtEventWatcher REQUIRED)
endif ()

add_executable(consume_test main.cpp)
target_link_libraries(consume_test PRIVATE QtEventWatcher::QtEventWatcherCore)
