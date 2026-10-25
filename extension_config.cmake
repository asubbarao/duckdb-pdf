# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(pdf
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# DuckDB 2.0 links an extension into its own binaries (the test runner, the
# shell) only when a config asks for it; duckdb_extension_load alone builds just
# the loadable .duckdb_extension. Unlinked, `require pdf` falls back to
# autoinstalling from build/<type>/repository/<version>/<platform>, which works
# in a local source build but not in CI's build against the prebuilt DuckDB
# library ("Linked into DuckDB: extensions [core_functions, parquet, json, icu]"),
# so every test file skipped at `require pdf` and CI still reported success.
# Linked, `require pdf` loads the code this tree just compiled, on every
# platform, with or without a prebuilt DuckDB.
duckdb_extension_statically_link(pdf)

# httpfs is deliberately NOT built here: it lives out of tree, so building it
# would mean cloning and compiling a second repo for every build. The network
# tests get the published httpfs instead; see the repo Makefile.
