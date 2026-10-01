# ============================================================
# TestEventWatcher - 冒烟测试
# ============================================================

add_executable(TestEventWatcher
    TestEventWatcher.cpp
)

target_link_libraries(TestEventWatcher
    PRIVATE
        QtEventWatcherCore
)

# 白盒冒烟：直接验证内部模块（告警抑制/看门狗/栈采集/记录存储/IPC/导出）
target_include_directories(TestEventWatcher
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/config
        ${PROJECT_SOURCE_DIR}/src/event
        ${PROJECT_SOURCE_DIR}/src/ipc
        ${PROJECT_SOURCE_DIR}/src/runtime
)

qew_deploy_sql_plugins(TestEventWatcher)	# V3 C1：QSQLITE 运行件随构建部署

add_test(NAME TestEventWatcher COMMAND TestEventWatcher)

# ============================================================
# UnitTests - V6 Q1 纯逻辑单元测试（自写 QEWT 断言，无 QtTest 依赖）
# 覆盖：解析族 / AlarmSuppressor 状态机 / kindOf 门控 / 环形+游标 / WatchConfig
# ============================================================

add_executable(UnitTests
    unit/UnitTests.cpp
)

target_link_libraries(UnitTests
    PRIVATE
        QtEventWatcherCore
)

target_include_directories(UnitTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/config
        ${PROJECT_SOURCE_DIR}/src/event
        ${PROJECT_SOURCE_DIR}/src/logging
)

add_test(NAME UnitTests COMMAND UnitTests)

# ============================================================
# BenchmarkEventWatcher - 性能基准（V2 Phase D，手动运行，不进 CTest）
# 用法：BenchmarkEventWatcher <off|event|slow|metacall_on|metacall_off>
# ============================================================

add_executable(BenchmarkEventWatcher
    benchmark/BenchmarkEventWatcher.cpp
)

target_link_libraries(BenchmarkEventWatcher
    PRIVATE
        QtEventWatcherCore
)

# 白盒基准：record() 微基准直用 EventStatistics（event 内部头）
target_include_directories(BenchmarkEventWatcher
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/event
)

# ============================================================
# ScoutDashboardTests - V7 S3 scout-dashboard 数据层单元测试
# （纯 Qt::Core 消费方：DashboardModel 解析/归类/冻结分段/relMs 跨午夜）
# ============================================================

add_executable(ScoutDashboardTests
    unit/ScoutDashboardTests.cpp
    ${PROJECT_SOURCE_DIR}/examples/scout-dashboard/DashboardModel.cpp
)

target_link_libraries(ScoutDashboardTests
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
)

target_include_directories(ScoutDashboardTests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/examples/scout-dashboard
)

add_test(NAME ScoutDashboardTests COMMAND ScoutDashboardTests)
