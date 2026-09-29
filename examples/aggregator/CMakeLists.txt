# ============================================================
# aggregator - V4 D2 中心收集器（多进程聚合控制台）
# ============================================================

add_executable(aggregator
    main.cpp
    AggregationReporter.cpp
    AggregationReporter.h
)

target_link_libraries(aggregator
    PRIVATE
        QtEventWatcherCore
)
