# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(semantic_layer
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# TPC-H data generator, used by the Snowflake-example tests and benchmarks
duckdb_extension_load(tpch)

# Linked into the shell (./build/release/duckdb); loaded extensions are otherwise only built
duckdb_extension_statically_link(semantic_layer tpch)

# DuckLake (an upstream main commit that builds against DuckDB v2.0.0), for the incremental materialization tests and
# benchmarks. It needs roaring from vcpkg, so it is only built with vcpkg (CI, or VCPKG_TOOLCHAIN_PATH=... make);
# without it the DuckLake tests are skipped.
if (VCPKG_BUILD)
    duckdb_extension_load(ducklake
        GIT_URL https://github.com/duckdb/ducklake
        GIT_TAG 37d4fe4d66fd55f808ae96a1b3b7e67c559a56a9
    )
    duckdb_extension_statically_link(ducklake)
endif()
