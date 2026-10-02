# ============================================================
# QtEventWatcher Core
# ============================================================

set(QT_EVENT_WATCHER_SOURCES)

# ------------------------------------------------------------
# Modules
# ------------------------------------------------------------

add_subdirectory(application)
add_subdirectory(config)
add_subdirectory(event)
add_subdirectory(ipc)
add_subdirectory(logging)
add_subdirectory(qss)
add_subdirectory(runtime)


# ============================================================
# Core Library
# ============================================================

add_library(QtEventWatcherCore STATIC
    ${QT_EVENT_WATCHER_SOURCES}
)


# ============================================================
# Include Directories（V3 D2 API 收紧）
# ============================================================

# 公共面只保留文档承诺的接入头：CusApplication.h（application）、
# WatchLogger.h / WatchLogCapture.h（logging）、QssStyleWatcher.h（qss）、
# WatchEventInfo.h（include/qt_event_watcher）。
# V4 C1：config 从公共面移除——CusApplication.h 改 unique_ptr<WatchConfig>
# 藏匿实现，WatchConfig.h 不再经公共头传播；内部实现目录（config、event
# 看门狗/抑制器/栈采集、ipc 服务、runtime 记录存储/导出）一律 PRIVATE——
# consumer 白盒需求（tests / benchmark / demo）由各自目标显式声明 include
# 目录，不走库的公共传播。
target_include_directories(QtEventWatcherCore
    PUBLIC
        # BUILD_INTERFACE 包装：install(EXPORT) 禁止 INTERFACE 属性携带
        # 源码树前缀绝对路径（Ninja/MinGW 生成器硬校验）；安装态头统一在
        # <prefix>/include（INSTALL_INTERFACE），扁平布局与构建态一致
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/application>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/logging>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/qss>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/qt_event_watcher>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_include_directories(QtEventWatcherCore
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/config
        ${PROJECT_SOURCE_DIR}/src/event
        ${PROJECT_SOURCE_DIR}/src/ipc
        ${PROJECT_SOURCE_DIR}/src/runtime
)

if (QT_VERSION_MAJOR EQUAL 6)
    # Qt6：私有头（qobject_p.h 等）经 CorePrivate 传播目标提供，
    # 路径同样自带版本号（.../include/QtCore/6.5.3/QtCore/private）
    target_link_libraries(QtEventWatcherCore PRIVATE Qt6::CorePrivate)
else ()
    # Qt5：Qt5Core_PRIVATE_INCLUDE_DIRS 路径自带版本号（如 5.15.2），
    # 换 Qt 版本必须重新适配（PRD 06：版本适配能力，非永久 ABI 保证）
    target_include_directories(QtEventWatcherCore
        PRIVATE
            ${Qt5Core_PRIVATE_INCLUDE_DIRS}
    )
endif ()

# ============================================================
# Qt Dependencies / spdlog
# ============================================================

target_link_libraries(QtEventWatcherCore
    PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Network
        Qt${QT_VERSION_MAJOR}::Sql		# V3 C1：SQLite 持久化导出（QSQLITE 驱动随 Qt 发行）
        spdlog::spdlog
)

# Windows 系统库：MinGW 下 K32* 映射到 psapi.dll 无前缀导出（见
# WindowsModuleEnumerator.cpp），MSVC 经 kernel32 内联转发无需显式链接
if (WIN32)
    target_link_libraries(QtEventWatcherCore PRIVATE psapi)
endif ()

# Linux：StackCapture 用 dladdr（glibc 2.34 前符号在 libdl，如麒麟 V10 的
# glibc 2.28）；CMAKE_DL_LIBS 在无需显式 dl 的平台上为空，跨发行版安全
if (UNIX AND NOT APPLE)
    target_link_libraries(QtEventWatcherCore PRIVATE ${CMAKE_DL_LIBS})
endif ()

# V3 A2：风暴抑制静默条走 QEW_LOG_DEBUG（运行时门控）。spdlog 默认
# SPDLOG_ACTIVE_LEVEL=INFO 会把 DEBUG 宏编译期剥离为空，必须显式放开；
# PUBLIC 使测试/Demo 等 consumer TU 的宏展开同样生效
target_compile_definitions(QtEventWatcherCore PUBLIC SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

# ============================================================
# Install（V4 C2：find_package 分发模型）
# ============================================================

install(TARGETS QtEventWatcherCore EXPORT QtEventWatcherTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(EXPORT QtEventWatcherTargets
    FILE QtEventWatcherTargets.cmake
    NAMESPACE QtEventWatcher::
    DESTINATION lib/cmake/QtEventWatcher
)

# 公共面头（application / logging / qss / include 四目录，V3 D2 定义的
# 公共面）统一扁平安装到 include/ 根——与构建态扁平 include 形式一致，
# 配合 INSTALL_INTERFACE:include；event / ipc / runtime / config 内部头
# 一律不装——consumer 只允许接入头，白盒属主仓特权
install(DIRECTORY
    ${PROJECT_SOURCE_DIR}/src/application/
    ${PROJECT_SOURCE_DIR}/src/logging/
    ${PROJECT_SOURCE_DIR}/src/qss/
    ${PROJECT_SOURCE_DIR}/include/qt_event_watcher/
    DESTINATION include
    FILES_MATCHING
        PATTERN "*.h"
)