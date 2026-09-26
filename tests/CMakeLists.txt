function(nessodb_enable_test_warnings target)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
        )
    endif()
endfunction()

function(nessodb_add_test name source)
    set(target "nessodb_${name}_test")

    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE NessoDB::nessodb)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    nessodb_enable_test_warnings(${target})

    add_test(NAME ${name} COMMAND ${target})
endfunction()

nessodb_add_test(lexer sql/lexer_test.cpp)
nessodb_add_test(parser sql/parser_test.cpp)
nessodb_add_test(binder binder/binder_test.cpp)
nessodb_add_test(query engine/query_test.cpp)

nessodb_add_test(predicate_evaluator execution/predicate_evaluator_test.cpp)
nessodb_add_test(expression_evaluator execution/expression_evaluator_test.cpp)
nessodb_add_test(aggregate_state execution/aggregate_state_test.cpp)
nessodb_add_test(
    global_aggregate_operator
    execution/global_aggregate_operator_test.cpp
)
nessodb_add_test(
    hash_aggregate_operator
    execution/hash_aggregate_operator_test.cpp
)
nessodb_add_test(
    nested_loop_join_operator
    execution/nested_loop_join_operator_test.cpp
)
nessodb_add_test(select_executor execution/select_executor_test.cpp)
nessodb_add_test(
    physical_operator_builder
    execution/physical_operator_builder_test.cpp
)

nessodb_add_test(logical_planner planner/logical_planner_test.cpp)
nessodb_add_test(physical_planner planner/physical_planner_test.cpp)
nessodb_add_test(
    physical_plan_formatter
    planner/physical_plan_formatter_test.cpp
)

add_test(NAME cli_select COMMAND nessodb_cli ":memory:" "SELECT 1, 2;")
set_tests_properties(cli_select PROPERTIES
    PASS_REGULAR_EXPRESSION "1 \\| 2"
)

add_test(NAME cli_text_select COMMAND nessodb_cli ":memory:" "SELECT 'Alice';")
set_tests_properties(cli_text_select PROPERTIES
    PASS_REGULAR_EXPRESSION "Alice"
)

add_test(NAME cli_null_select COMMAND nessodb_cli ":memory:" "SELECT NULL;")
set_tests_properties(cli_null_select PROPERTIES
    PASS_REGULAR_EXPRESSION "NULL"
)

add_test(
    NAME cli_table_scan
    COMMAND nessodb_cli
        ":memory:"
        "CREATE TABLE users(id INT, name TEXT);"
        "INSERT INTO users VALUES (1, 'Alice');"
        "INSERT INTO users VALUES (2, NULL);"
        "SELECT * FROM users;"
)
set_tests_properties(cli_table_scan PROPERTIES
    PASS_REGULAR_EXPRESSION "1 \\| Alice.*2 \\| NULL"
)

nessodb_add_test(catalog catalog/catalog_test.cpp)

nessodb_add_test(heap storage/heap_test.cpp)
nessodb_add_test(page storage/page_test.cpp)
nessodb_add_test(page_file storage/page_file_test.cpp)
nessodb_add_test(database_file storage/database_file_test.cpp)
nessodb_add_test(buffer_pool storage/buffer_pool_test.cpp)
nessodb_add_test(record_codec storage/record_codec_test.cpp)
nessodb_add_test(table_heap storage/table_heap_test.cpp)
nessodb_add_test(catalog_store storage/catalog_store_test.cpp)
nessodb_add_test(storage_manager storage/storage_manager_test.cpp)
nessodb_add_test(undo_log storage/undo_log_test.cpp)

nessodb_add_test(key_codec index/key_codec_test.cpp)
nessodb_add_test(btree_page index/btree_page_test.cpp)
nessodb_add_test(btree index/btree_test.cpp)

nessodb_add_test(wal recovery/wal_test.cpp)

find_package(Threads REQUIRED)
nessodb_add_test(transaction transaction/transaction_test.cpp)
target_link_libraries(nessodb_transaction_test PRIVATE Threads::Threads)
