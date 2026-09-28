# ============================================================
# basic_demo - QtEventWatcher 基础使用示例
# ============================================================

set(BASIC_DEMO_SOURCES
    main.cpp
    MainWindow.cpp
    MainWindow.h
    StyleTokens.cpp
    StyleTokens.h
)

add_executable(basic_demo WIN32
    ${BASIC_DEMO_SOURCES}
)

target_include_directories(basic_demo
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(basic_demo
    PRIVATE
        QtEventWatcherCore
)
