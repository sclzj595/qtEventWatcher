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
# Include Directories
# ============================================================

# include/qt_event_watcher 存放真实公共头（WatchEventInfo.h）；
# 内部模块目录在前，供实现文件以短名 include 各自模块头
target_include_directories(QtEventWatcherCore
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src/application
        ${PROJECT_SOURCE_DIR}/src/config
        ${PROJECT_SOURCE_DIR}/src/event
        ${PROJECT_SOURCE_DIR}/src/ipc
        ${PROJECT_SOURCE_DIR}/src/logging
        ${PROJECT_SOURCE_DIR}/src/qss
        ${PROJECT_SOURCE_DIR}/src/runtime
        ${PROJECT_SOURCE_DIR}/include/qt_event_watcher
        ${PROJECT_SOURCE_DIR}/include
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
        spdlog::spdlog
)

# Windows 系统库：MinGW 下 K32* 映射到 psapi.dll 无前缀导出（见
# WindowsModuleEnumerator.cpp），MSVC 经 kernel32 内联转发无需显式链接
if (WIN32)
    target_link_libraries(QtEventWatcherCore PRIVATE psapi)
endif ()

# ============================================================
# Install
# ============================================================

install(TARGETS QtEventWatcherCore
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY
    ${PROJECT_SOURCE_DIR}/include/qt_event_watcher
    DESTINATION include
    FILES_MATCHING
        PATTERN "*.h"
)