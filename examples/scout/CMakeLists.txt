# ============================================================
# scout - 跨栈程序卡顿外部探针（V7 S1 / 产品线 B，console）
# T1 窗口冻结 + T1b CPU 启发，进程外检测任意 Windows 桌面程序；
# 观测记录复用自监控日志行协议 → UplinkClient → aggregator
# ============================================================

set(SCOUT_SOURCES
    main.cpp
    WindowFreezeProber.cpp
    WindowFreezeProber.h
    CpuSampler.cpp
    CpuSampler.h
    TargetResolver.h
)

add_executable(scout
    ${SCOUT_SOURCES}
)

target_include_directories(scout
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        # 白盒（V4 C1/D2 收紧后的 PRIVATE 面；探针复用配置与上行链路，
        # 与 tests/examples 白盒口径一致）
        ${PROJECT_SOURCE_DIR}/src/config
        ${PROJECT_SOURCE_DIR}/src/ipc
)

target_link_libraries(scout
    PRIVATE
        QtEventWatcherCore
)
