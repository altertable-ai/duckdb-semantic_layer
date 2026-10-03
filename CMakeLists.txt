cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME semantic_views)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)
include_directories(BEFORE third_party/duckdb_peg_parser/include)
include_directories(third_party/duckdb_peg_parser)

set(EXTENSION_SOURCES
    src/semantic_views_extension.cpp
    src/semantic_views_binding.cpp
    src/semantic_views_functions.cpp
    src/semantic_ddl_functions.cpp
    src/semantic_show_functions.cpp
    src/semantic_query_functions.cpp
    src/semantic_lowering.cpp
    src/parsed_semantic_view.cpp
    src/semantic_analysis.cpp
    src/semantic_bridge.cpp
    src/semantic_join_scope.cpp
    src/semantic_markers.cpp
    src/semantic_model.cpp
    src/semantic_model_builder.cpp
    src/semantic_query_compiler.cpp
    src/semantic_sql_rewrite.cpp
    src/semantic_statement_visitor.cpp
    src/semantic_view_registry.cpp
    src/parser/semantic_views_parser.cpp)

# Vendored DuckDB PEG parser (appends to EXTENSION_SOURCES)
add_subdirectory(third_party/duckdb_peg_parser)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
