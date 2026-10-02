# ============================================================
# scout - 跨栈程序卡顿外部探针（V7 / 产品线 B，console）
# T1 窗口冻结 + T1b CPU 启发，进程外检测任意 Windows 桌面程序；
# T2 CDP 长任务（可选，Qt WebSockets 依赖）；
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

# S2：CDP 长任务探针——可选依赖 Qt WebSockets（Qt5/Qt6 官方二进制均自带）。
# 缺失时禁用该探针并提示，不阻断构建（main.cpp 经 QEWT_SCOUT_CDP 条件接线）。
find_package(Qt${QT_VERSION_MAJOR} COMPONENTS WebSockets QUIET)
if (TARGET Qt${QT_VERSION_MAJOR}::WebSockets)
    target_sources(scout PRIVATE
        CdpLongTaskProber.cpp
        CdpLongTaskProber.h
    )
    target_compile_definitions(scout PRIVATE QEWT_SCOUT_CDP=1)
    target_link_libraries(scout PRIVATE Qt${QT_VERSION_MAJOR}::WebSockets)
else()
    message(STATUS "scout: Qt${QT_VERSION_MAJOR}::WebSockets not found, "
                   "CDP long-task prober disabled")
endif()

# L3：X11 冻结探针（Linux 可选依赖——X11 桌面/麒麟 UKUI 场景）。Wayland
# native 无外部探针协议，X11 缺失时禁用该探针不阻断构建，沿 WebSockets 先例
if (UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if (TARGET X11::X11)
        target_compile_definitions(scout PRIVATE QEWT_SCOUT_X11=1)
        target_link_libraries(scout PRIVATE X11::X11)
    else()
        message(STATUS "scout: X11 not found, window freeze prober disabled")
    endif()
endif()
