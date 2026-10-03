PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=semantic_views
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# AGENTS.md: always build with Ninja (override with GEN= to use the CMake default).
# Wasm targets in extension-ci-tools still invoke `emmake make`, which needs the
# Unix Makefiles generator - keep GEN unset for those builds.
ifneq ($(filter wasm_mvp wasm_eh wasm_threads,$(MAKECMDGOALS) $(DUCKDB_PLATFORM)),)
GEN ?=
else
GEN ?= ninja
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
