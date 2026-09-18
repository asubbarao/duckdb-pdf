# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(pdf
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
#
# httpfs is deliberately NOT linked here: in DuckDB 1.5 it lives out of tree, so
# loading it would mean cloning and building a second repo for every build.
# test/sql/read_pdf_url.test therefore begins with `require httpfs` and is
# skipped unless the runner's DuckDB already provides it. See its header.