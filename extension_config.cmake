# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(semantic_layer
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# TPC-H data generator, used by the Snowflake-example tests and benchmarks
duckdb_extension_load(tpch)

# DuckLake (the commit DuckDB v1.5.5 ships), for the incremental materialization tests and benchmarks. It needs
# roaring from vcpkg, so it is only built with vcpkg (CI, or VCPKG_TOOLCHAIN_PATH=... make); without it the DuckLake
# tests are skipped.
if (VCPKG_BUILD)
    duckdb_extension_load(ducklake
        GIT_URL https://github.com/duckdb/ducklake
        GIT_TAG d8a1881e22516ea3d186d73e83c65fe5bd1a1dc4
    )
endif()
