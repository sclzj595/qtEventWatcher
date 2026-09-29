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
        # 白盒展示：GUI 直接消费记录导出/诊断/运行环境（runtime）、
        # MetaCallFilter 预置规则演示（event）、导出接口的 WatchConfig
        # 参数解引用（config，V4 C1 起库公共面不再传播）
        ${PROJECT_SOURCE_DIR}/src/runtime
        ${PROJECT_SOURCE_DIR}/src/event
        ${PROJECT_SOURCE_DIR}/src/config
)

target_link_libraries(basic_demo
    PRIVATE
        QtEventWatcherCore
)

qew_deploy_sql_plugins(basic_demo)	# V3 C1：.db 导出在部署形态下开箱可用
