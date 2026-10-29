cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME ofexcel)

# Dependencies come from vcpkg (vcpkg.json). fusion_scanner's own dependencies
# (curl, openssl, tinyxml2) are listed there too, because it is built in this tree.
find_package(httplib CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# The C++ standard is set per target with target_compile_features, never through
# CMAKE_CXX_STANDARD: DuckDB owns that cache variable, and raising it from an
# extension splits DuckDB's own targets between two standards (see ofquack's
# CMakeLists.txt for the link failure that causes on Linux).

include_directories(src/include)

set(EXTENSION_SOURCES
        src/ofexcel_extension.cpp
        src/sql_functions.cpp
        src/version.cpp
        src/db_util.cpp
        src/meta/migrations.cpp
        src/server/request_guard.cpp
        src/server/json_body.cpp
        src/server/http_server.cpp
        src/server/routes_health.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# PRIVATE, so the requirement does not propagate to DuckDB's own targets.
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)
target_link_libraries(${EXTENSION_NAME} httplib::httplib)
target_link_libraries(${LOADABLE_EXTENSION_NAME} httplib::httplib)

# Headers of the extensions ofexceld and the tests load explicitly. fusion_scanner
# is declared in another directory, so its source directory is resolved when the
# build is generated rather than probed now.
set(OFEXCEL_FUSION_SCANNER_SRC $<TARGET_PROPERTY:fusion_scanner_extension,SOURCE_DIR>)
set(OFEXCEL_LINKED_EXTENSION_INCLUDES
        ${CMAKE_SOURCE_DIR}/extension/core_functions/include
        ${OFEXCEL_FUSION_SCANNER_SRC}/src/include
)

# DuckDB 2.0 resolves which extensions are linked only after every extension
# directory has been processed, so link_extension_libraries sees an empty list
# here. The three archives are named instead, and every binary loads them
# explicitly with LoadStaticExtension.
set(OFEXCEL_LINKED_EXTENSIONS ${EXTENSION_NAME} fusion_scanner_extension core_functions_extension)

# ofexceld: the daemon. One binary with DuckDB and all three extensions inside.
add_executable(ofexceld src/main.cpp)
target_link_libraries(ofexceld PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS})
target_include_directories(ofexceld PRIVATE src/include ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
target_compile_features(ofexceld PRIVATE cxx_std_17)

option(OFEXCEL_BUILD_TESTS "Build the ofexcel C++ tests" ON)
if(OFEXCEL_BUILD_TESTS)
  enable_testing()

  # Pure: no DuckDB, no network. Runs in well under a second.
  add_executable(ofexcel_guard_test test/cpp/guard_test.cpp src/server/request_guard.cpp)
  target_include_directories(ofexcel_guard_test PRIVATE src/include)
  target_compile_features(ofexcel_guard_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_guard_test COMMAND ofexcel_guard_test)

  # Starts the HTTP server inside an in-memory database and talks to it over
  # 127.0.0.1 on an ephemeral port.
  add_executable(ofexcel_server_test test/cpp/server_test.cpp)
  target_link_libraries(ofexcel_server_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS} httplib::httplib)
  target_include_directories(ofexcel_server_test PRIVATE src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
  target_compile_features(ofexcel_server_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_server_test COMMAND ofexcel_server_test)

  # Drives oracle_fusion_query through ofquack's transport seam with scripted
  # SOAP responses: pins the ofquack behaviour CONTRACTS.md §5 relies on,
  # without a Fusion instance.
  add_executable(ofexcel_fusion_seam_test test/cpp/fusion_seam_test.cpp)
  target_link_libraries(ofexcel_fusion_seam_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS})
  target_include_directories(ofexcel_fusion_seam_test PRIVATE src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES}
                             ${OFEXCEL_FUSION_SCANNER_SRC}/src)
  target_compile_features(ofexcel_fusion_seam_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_fusion_seam_test COMMAND ofexcel_fusion_seam_test)

  # Checks against a real Fusion instance (tools/live_probe.cpp). Built with the
  # tests but deliberately not registered with ctest: it needs SSO and a network.
  add_executable(ofexcel_live_probe tools/live_probe.cpp)
  target_link_libraries(ofexcel_live_probe PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS})
  target_include_directories(ofexcel_live_probe PRIVATE src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
  target_compile_features(ofexcel_live_probe PRIVATE cxx_std_17)
endif()

install(
        TARGETS ${EXTENSION_NAME}
        EXPORT "${DUCKDB_EXPORT_SET}"
        LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
        ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
