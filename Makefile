PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=pdf
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# httpfs lives out of tree in DuckDB 1.5, so it is not linked into the test
# binary; the sqllogictest runner will only load it when autoloading is allowed.
# Without this the URL tests are skipped at `require httpfs` and never run.
export DUCKDB_TEST_AUTOLOADING=all

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
