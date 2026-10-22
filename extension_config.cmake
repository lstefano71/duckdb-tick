# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(tick
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# ICU provides time zone support (the TimeZone setting and named zones)
duckdb_extension_load(icu)
