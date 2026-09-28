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

add_test(NAME TestEventWatcher COMMAND TestEventWatcher)

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
