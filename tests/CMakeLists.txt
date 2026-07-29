add_executable(minidb_lexer_test
    sql/lexer_test.cpp
)
target_link_libraries(minidb_lexer_test PRIVATE MiniDB::minidb)
target_include_directories(minidb_lexer_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
set_target_properties(minidb_lexer_test PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(minidb_lexer_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(minidb_lexer_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

add_test(NAME lexer COMMAND minidb_lexer_test)

add_executable(minidb_parser_test
    sql/parser_test.cpp
)
target_link_libraries(minidb_parser_test PRIVATE MiniDB::minidb)
target_include_directories(minidb_parser_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
set_target_properties(minidb_parser_test PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(minidb_parser_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(minidb_parser_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

add_test(NAME parser COMMAND minidb_parser_test)

add_executable(minidb_binder_test
    binder/binder_test.cpp
)
target_link_libraries(minidb_binder_test PRIVATE MiniDB::minidb)
target_include_directories(minidb_binder_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
set_target_properties(minidb_binder_test PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(minidb_binder_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(minidb_binder_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

add_test(NAME binder COMMAND minidb_binder_test)

add_executable(minidb_query_test
    engine/query_test.cpp
)
target_link_libraries(minidb_query_test PRIVATE MiniDB::minidb)
target_include_directories(minidb_query_test PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
set_target_properties(minidb_query_test PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(minidb_query_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(minidb_query_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

add_test(NAME query COMMAND minidb_query_test)
add_test(NAME cli_select COMMAND minidb_cli "SELECT 1, 2;")
set_tests_properties(cli_select PROPERTIES
    PASS_REGULAR_EXPRESSION "1 \\| 2"
)
add_test(NAME cli_text_select COMMAND minidb_cli "SELECT 'Alice';")
set_tests_properties(cli_text_select PROPERTIES
    PASS_REGULAR_EXPRESSION "Alice"
)
