# ============================================================
# scout-dashboard - Scout 独立可视化仪表盘（V7 S3 线，console）
# 读取 aggregator --out JSON 快照 → 单文件自包含 HTML
# 纯文件消费方：数据契约 = aggregator JSON 形状，
# 不链 QtEventWatcherCore（零 QtSql/WebSockets/Widgets）
# ============================================================

set(SCOUT_DASHBOARD_SOURCES
    main.cpp
    DashboardModel.cpp
    DashboardModel.h
    DashboardHtml.cpp
    DashboardHtml.h
)

add_executable(scout-dashboard
    ${SCOUT_DASHBOARD_SOURCES}
)

target_link_libraries(scout-dashboard
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
)

# S3c：JS 交互版资源内嵌（assets.qrc 存在时启用，AUTORCC 编译进 exe；
# QEWT_DASHBOARD_EMBED 未定义时保持纯静态双形态共存）
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets.qrc")
    target_sources(scout-dashboard PRIVATE assets.qrc)
    target_compile_definitions(scout-dashboard PRIVATE QEWT_DASHBOARD_EMBED=1)
endif()
