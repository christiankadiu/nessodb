cmake_minimum_required(VERSION 3.21)

project(
    MiniDB
    DESCRIPTION "A relational database engine written in modern C++"
    LANGUAGES CXX
)

include(CTest)

add_library(minidb
    src/binder/binder.cpp
    src/catalog/catalog.cpp
    src/engine/database.cpp
    src/engine/query.cpp
    src/sql/lexer.cpp
    src/sql/parser.cpp
    src/storage/access/in_memory_heap.cpp
)
add_library(MiniDB::minidb ALIAS minidb)

target_compile_features(minidb PUBLIC cxx_std_23)
target_include_directories(minidb
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
)
set_target_properties(minidb PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(minidb PRIVATE /W4 /permissive-)
else()
    target_compile_options(minidb PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

add_executable(minidb_cli
    apps/cli/main.cpp
)
target_link_libraries(minidb_cli PRIVATE MiniDB::minidb)
target_include_directories(minidb_cli PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
set_target_properties(minidb_cli PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME minidb
)

if(MSVC)
    target_compile_options(minidb_cli PRIVATE /W4 /permissive-)
else()
    target_compile_options(minidb_cli PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
