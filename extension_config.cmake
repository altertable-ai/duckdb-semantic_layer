# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(semantic_views
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# TPC-H data generator, used by the Snowflake-example tests and benchmarks
duckdb_extension_load(tpch)
