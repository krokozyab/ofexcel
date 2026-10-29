# Included by DuckDB's build system. Names the extensions this build compiles and
# links into ofexceld, the DuckDB shell and the unittest runner.

# EXTENSION_VERSION is stated rather than derived from `git log`, so a checkout
# without history (or a fresh clone before its first commit) still configures.
# Keep it equal to PRODUCT_VERSION in src/version.cpp.
duckdb_extension_load(ofexcel
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    EXTENSION_VERSION 0.1.0-dev
)

# fusion_scanner (ofquack) is pinned to the commit built against the same DuckDB
# as the duckdb submodule. OFQUACK_SOURCE_DIR builds a local checkout instead; it
# must be at a commit that builds against that DuckDB too.
#
# ofquack's own C++ tests are not built here: they belong to its repository.
set(FUSION_SCANNER_BUILD_TESTS OFF CACHE BOOL "Build the fusion_scanner C++ unit tests" FORCE)
if(DEFINED ENV{OFQUACK_SOURCE_DIR})
    duckdb_extension_load(fusion_scanner
        SOURCE_DIR $ENV{OFQUACK_SOURCE_DIR}
    )
else()
    duckdb_extension_load(fusion_scanner
        GIT_URL https://github.com/krokozyab/ofquack
        GIT_TAG 15be8cc3613186a26bccc629cf3663df9b234fce
    )
endif()

# DuckDB 2.0 builds an extension and links it as separate decisions. Linking all
# three statically is what makes `require ofexcel` work in sqllogictests and gives
# the shell the same functions as ofexceld. core_functions holds list_value and
# the rest of what 1.5 linked by default.
if(COMMAND duckdb_extension_statically_link)
    duckdb_extension_statically_link(ofexcel fusion_scanner core_functions)
endif()
