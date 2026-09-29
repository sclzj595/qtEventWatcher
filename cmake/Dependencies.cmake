# cmake/Dependencies.cmake

include_guard(GLOBAL)

# ============================================================
# spdlog
# ============================================================

if (NOT TARGET spdlog::spdlog)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    # V4 C2：spdlog 随主包 install（自带 spdlogConfig.cmake），
    # QtEventWatcherConfig.cmake 经 find_dependency(spdlog CONFIG) 解析
    set(SPDLOG_INSTALL ON CACHE BOOL "" FORCE)

    # PROJECT_SOURCE_DIR 而非 CMAKE_SOURCE_DIR：add_subdirectory 接入模式下
    # （examples/consume-test 模式 2），CMAKE_SOURCE_DIR 指向消费工程，
    # project(QtEventWatcher) 后 PROJECT_SOURCE_DIR 恒指本仓库根
    add_subdirectory(
        ${PROJECT_SOURCE_DIR}/third_party/spdlog-1.x
        ${CMAKE_BINARY_DIR}/third_party/spdlog
    )
    # 注意不可 EXCLUDE_FROM_ALL：该标记会把子目录从顶层 install 序列剔除，
    # SPDLOG_INSTALL=ON 的 install 规则虽注册但 cmake --install 不会执行
endif ()