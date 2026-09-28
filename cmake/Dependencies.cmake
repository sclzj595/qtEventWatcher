# cmake/Dependencies.cmake

include_guard(GLOBAL)

# ============================================================
# spdlog
# ============================================================

if (NOT TARGET spdlog::spdlog)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)

    add_subdirectory(
        ${CMAKE_SOURCE_DIR}/third_party/spdlog-1.x
        ${CMAKE_BINARY_DIR}/third_party/spdlog
        EXCLUDE_FROM_ALL
    )
endif ()