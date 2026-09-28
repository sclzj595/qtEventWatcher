cmake_minimum_required(VERSION 3.16)

project(QtEventWatcher
    VERSION 0.1.0
    LANGUAGES CXX
)

# ============================================================
# C++ Standard
# ============================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# ============================================================
# Build Type (VS multi-config: only Release)
# ============================================================

if (NOT EXISTS "${CMAKE_SOURCE_DIR}/.git" OR WIN32)
    # Visual Studio 多配置生成器下仅生成 Release 配置
    set(CMAKE_CONFIGURATION_TYPES "Release" CACHE STRING "Configurations" FORCE)
endif ()

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)

# Release 编译选项（MSVC）
if (MSVC)
    add_compile_options(
        /MP              # 并行编译
        /utf-8           # 源码 UTF-8（含中文注释）
        /W3
    )
    # Release 下消除 CRT 安全告警
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif ()


# ============================================================
# Qt
# ============================================================

# Qt 前缀：命令行 -DCMAKE_PREFIX_PATH 优先；未指定时默认 Qt 5.15.2
if (NOT CMAKE_PREFIX_PATH)
    set(CMAKE_PREFIX_PATH
        "F:/IDE.2/Qt5.15.2/5.15.2/msvc2019_64"
    )
endif ()

# Qt6 优先探测、回退 Qt5（PRD 06：版本适配能力）。
# QT_VERSION_MAJOR 由探测结果给出（6 或 5），下游统一用
# Qt${QT_VERSION_MAJOR}:: 引用目标，避免硬编码
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS
    Core
    Widgets
    Network
)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS
    Core
    Widgets
    Network
)


# ============================================================
# Global Build Options
# ============================================================

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/bin"
)

set(CMAKE_LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/lib"
)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY
    "${CMAKE_BINARY_DIR}/lib"
)


# ============================================================
# Project Options
# ============================================================

option(QT_EVENT_WATCHER_BUILD_TESTS
    "Build QtEventWatcher tests"
    ON
)

option(QT_EVENT_WATCHER_BUILD_EXAMPLES
    "Build QtEventWatcher examples"
    ON
)


# ============================================================
# Dependencies
# ============================================================

include(cmake/Dependencies.cmake)


# ============================================================
# Source
# ============================================================

add_subdirectory(src)


# ============================================================
# Tests
# ============================================================

if(QT_EVENT_WATCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()


# ============================================================
# Examples
# ============================================================

if(QT_EVENT_WATCHER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()


# ============================================================
# Install
# ============================================================

# 库和头文件由 src/CMakeLists.txt 安装；此处指定安装前缀默认值
if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Install prefix" FORCE)
endif ()