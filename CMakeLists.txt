cmake_minimum_required(VERSION 3.16)
project(SimpleDB LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SIMPLEDB_CORE_SOURCES
    common/durable_io.cpp
    storage/disk_manager.cpp
    storage/slotted_page.cpp
    storage/free_space_map.cpp
    storage/relation_manager.cpp
    buffer/clock_replacer.cpp
    buffer/buffer_pool_manager.cpp
    access/heap_file.cpp
    access/heap_file_iterator.cpp
    access/btree_page.cpp
    access/btree_core.cpp
    access/btree_insert.cpp
    access/btree_delete_recursive.cpp
    access/btree_delete_entry.cpp
    access/index.cpp
    access/composite_key_codec.cpp
    access/btree_iterator.cpp
    access/generic_btree_helpers.cpp
    access/generic_btree_core.cpp
    access/generic_btree_modify.cpp
    access/generic_btree_scan.cpp
    execution/operators_common.cpp
    execution/operators_scan.cpp
    execution/operators_transform.cpp
    execution/operators_join.cpp
    execution/operators_ordering.cpp
    execution/operators_aggregate.cpp
    catalog/catalog_manager.cpp
    storage/table.cpp
    catalog/stats_catalog.cpp
    optimizer/plan_enumerator.cpp
    concurrency/lock_manager_helpers.cpp
    concurrency/lock_manager_common.cpp
    concurrency/lock_manager_acquire.cpp
    concurrency/lock_manager_release.cpp
    concurrency/lock_manager_deadlock.cpp
    concurrency/transaction_manager.cpp
    concurrency/page_retire_manager.cpp
    recovery/log_manager.cpp
    recovery/checkpoint_manager.cpp
    recovery/recovery_manager.cpp
    sql/lexer.cpp
    sql/parser.cpp
    sql/binder.cpp
    sql/planner_frontend_helpers.cpp
    sql/planner_frontend_runtime.cpp
    sql/planner_frontend_select.cpp
    sql/planner_frontend_write.cpp
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/access/generic_btree_page.cpp")
    list(APPEND SIMPLEDB_CORE_SOURCES
        access/generic_btree_page.cpp
    )
endif()

add_library(simpledb_core STATIC ${SIMPLEDB_CORE_SOURCES})
target_include_directories(simpledb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(simpledb_tools STATIC
    tools/consistency_checker.cpp
    tools/benchmark_runner.cpp
    tools/crash_harness.cpp
    recovery/fault_injector.cpp
)
target_include_directories(simpledb_tools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(simpledb_tools PUBLIC simpledb_core)

add_executable(simpledb main.cpp)
target_link_libraries(simpledb PRIVATE simpledb_core)

add_executable(db_bench tools/db_bench_main.cpp)
target_link_libraries(db_bench PRIVATE simpledb_core simpledb_tools)

add_executable(db_crash_demo tools/db_crash_demo_main.cpp)
target_link_libraries(db_crash_demo PRIVATE simpledb_core simpledb_tools)

add_executable(db_sql_shell tools/db_sql_shell_main.cpp)
target_link_libraries(db_sql_shell PRIVATE simpledb_core simpledb_tools)

add_executable(db_sql_regression tools/db_sql_regression_main.cpp)
target_link_libraries(db_sql_regression PRIVATE simpledb_core simpledb_tools)

add_executable(db_concurrency_regression tools/db_concurrency_regression_main.cpp)
target_link_libraries(db_concurrency_regression PRIVATE simpledb_core simpledb_tools)

add_executable(db_recovery_regression tools/db_recovery_regression_main.cpp)
target_link_libraries(db_recovery_regression PRIVATE simpledb_core simpledb_tools)
