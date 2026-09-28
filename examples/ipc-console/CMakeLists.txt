# ============================================================
# ipc_console - IPC 控制台（V2 Phase B 发送端示例）
# ============================================================

add_executable(ipc_console
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

target_link_libraries(ipc_console
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Network
)
