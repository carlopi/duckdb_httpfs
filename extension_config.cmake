# This file is included by DuckDB's build system. It specifies which extension to load

	
duckdb_extension_load(spatial
    DONT_LINK LOAD_TESTS
    GIT_URL https://github.com/carlopi/duckdb_spatial.git
    GIT_TAG main
    INCLUDE_DIR spatial/include
    TEST_DIR test/sql
    )
