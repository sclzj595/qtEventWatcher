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
        /W4              # V6 Q2：最高常规告警级（清零守门）
    )
    # Release 下消除 CRT 安全告警
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif ()

# V6 Q2：GCC/MinGW 告警收紧（Qt 头经 imported target -isystem 不受影响）
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-Wall -Wextra)
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
    Sql
)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS
    Core
    Widgets
    Network
    Sql
)


# ============================================================
# Sql 运行件部署（V3 C1）
# ============================================================

# 背景：当 exe 同目录存在部署拷贝的 Qt5Core.dll 时（windeployqt 产物），
# QLibraryInfo 插件路径随 Core DLL 位置重定位，而 windeployqt 默认不部署
# sqldrivers 类目 -> QSQLITE 驱动不可用。将 QtSql 运行库与 qsqlite 插件
# 拷贝到目标输出目录（exe 旁），app-dir 的 sqldrivers/ 子目录始终在
# Qt 插件搜索路径内，无需 QT_PLUGIN_PATH 环境变量。
# Qt5/Qt6 与 MSVC/MinGW 官方二进制布局一致：<prefix>/bin、<prefix>/plugins/sqldrivers。
function(qew_deploy_sql_plugins target)
    get_filename_component(_qt_prefix "${Qt${QT_VERSION_MAJOR}Sql_DIR}/../../.." ABSOLUTE)
    set(_sql_dll "${_qt_prefix}/bin/Qt${QT_VERSION_MAJOR}Sql.dll")
    set(_sqlite_plugin "${_qt_prefix}/plugins/sqldrivers/qsqlite.dll")
    if (EXISTS "${_sql_dll}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_sql_dll}" "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Deploying Qt${QT_VERSION_MAJOR}Sql.dll"
        )
    endif ()
    if (EXISTS "${_sqlite_plugin}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/sqldrivers"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_sqlite_plugin}" "$<TARGET_FILE_DIR:${target}>/sqldrivers/"
            COMMENT "Deploying qsqlite.dll plugin"
        )
    endif ()
endfunction()


# ============================================================
# Qt 运行时部署（V7.0.1）：windeployqt 基建化
# ============================================================

# 背景：此前 bin 目录的 Qt5Core/Gui/Widgets/Network 等 DLL 是历史手动
# windeployqt 产物，不受 CMake 管理——clean build 后全部丢失；且 scout 的
# QtWebSockets 依赖从未被任何部署覆盖（exe 旁缺 Qt5WebSockets.dll，双击
# 直启即 0xC0000135）。测试/e2e 脚本注入 PATH 掩盖了缺 DLL（动态库按 PATH
# 命中），无 PATH 的普通启动才暴露。
# 统一 POST_BUILD 调 windeployqt（与 qmake 同目录）部署目标实际链接的
# Qt DLL + 插件类目（platforms/imageformats/iconengines/styles），构建树
# 自包含可复现；--no-opengl-sw/--no-translations 裁剪非必需大件，与既有
# 手动产物布局一致（libEGL/libGLESv2 保留=ANGLE 默认）。
# sqldrivers 类目 windeployqt 默认不部署，仍由 qew_deploy_sql_plugins 补齐。
# MinGW 不接入（保持 PATH 注入方式，避免 libgcc 运行时部署行为差异）。
function(qew_deploy_qt_runtime target)
    if (WIN32 AND MSVC AND TARGET Qt${QT_VERSION_MAJOR}::qmake)
        get_target_property(_qmake_loc Qt${QT_VERSION_MAJOR}::qmake LOCATION)
        get_filename_component(_qt_bin "${_qmake_loc}" DIRECTORY)
        if (EXISTS "${_qt_bin}/windeployqt.exe")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${_qt_bin}/windeployqt.exe"
                    --no-translations
                    --no-system-d3d-compiler
                    --no-opengl-sw
                    --no-quick-import
                    "$<TARGET_FILE:${target}>"
                COMMENT "Deploying Qt runtime (windeployqt) -> ${target}"
            )
        endif ()
    endif ()
endfunction()


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

# find_package 分发模型（V4 C2）：Config + Version + Targets 三件套
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/QtEventWatcherConfig.cmake.in
    "${CMAKE_BINARY_DIR}/cmake/QtEventWatcherConfig.cmake"
    INSTALL_DESTINATION lib/cmake/QtEventWatcher
)
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/cmake/QtEventWatcherConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_BINARY_DIR}/cmake/QtEventWatcherConfig.cmake"
    "${CMAKE_BINARY_DIR}/cmake/QtEventWatcherConfigVersion.cmake"
    DESTINATION lib/cmake/QtEventWatcher
)