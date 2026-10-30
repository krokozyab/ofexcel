cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME ofexcel)

# Dependencies come from vcpkg (vcpkg.json). fusion_scanner's own dependencies
# (curl, openssl, tinyxml2) are listed there too, because it is built in this tree.
find_package(httplib CONFIG REQUIRED)
# The local CA (src/security/local_ca.cpp) and httplib's HTTPS server.
find_package(OpenSSL REQUIRED)

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
        src/paths.cpp
        src/catalog/utf8.cpp
        src/catalog/hashing.cpp
        src/catalog/contract_types.cpp
        src/catalog/query_file.cpp
        src/catalog/param_values.cpp
        src/catalog/result_types.cpp
        src/catalog/oracle_render.cpp
        src/run/local_types.cpp
        src/run/local_validator.cpp
        src/run/fusion_errors.cpp
        src/run/run_work.cpp
        src/run/oracle_run.cpp
        src/run/local_run.cpp
        src/run/preview.cpp
        src/run/run_engine.cpp
        src/meta/migrations.cpp
        src/meta/connections.cpp
        src/meta/query_registry.cpp
        src/meta/datasets.cpp
        src/meta/imports.cpp
        src/security/local_ca.cpp
        src/security/trust_store.cpp
        src/server/request_guard.cpp
        src/server/json_body.cpp
        src/server/http_server.cpp
        src/server/routes_health.cpp
        src/server/routes_connections.cpp
        src/server/routes_queries.cpp
        src/server/routes_runs.cpp
        src/server/routes_imports.cpp
        src/server/routes_addin.cpp
        src/server/routes_probe.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# PRIVATE, so the requirement does not propagate to DuckDB's own targets.
target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)
target_link_libraries(${EXTENSION_NAME} httplib::httplib OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} httplib::httplib OpenSSL::SSL OpenSSL::Crypto)

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
# src/daemon is the binary's own code, not the extension's: it loads the other
# two extensions explicitly.
set(OFEXCELD_SOURCES src/daemon/startup.cpp)
add_executable(ofexceld src/main.cpp ${OFEXCELD_SOURCES})
target_link_libraries(ofexceld PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS})
target_include_directories(ofexceld PRIVATE src src/include ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
target_compile_features(ofexceld PRIVATE cxx_std_17)

option(OFEXCEL_BUILD_TESTS "Build the ofexcel C++ tests" ON)
if(OFEXCEL_BUILD_TESTS)
  enable_testing()

  # Pure: no DuckDB, no network. Runs in well under a second.
  add_executable(ofexcel_guard_test test/cpp/guard_test.cpp src/server/request_guard.cpp)
  target_include_directories(ofexcel_guard_test PRIVATE src/include)
  target_compile_features(ofexcel_guard_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_guard_test COMMAND ofexcel_guard_test)

  # Pure as well: the local CA and the server certificate, OpenSSL only, in a
  # temporary directory.
  add_executable(ofexcel_tls_test test/cpp/tls_test.cpp src/security/local_ca.cpp src/paths.cpp)
  target_link_libraries(ofexcel_tls_test PRIVATE OpenSSL::Crypto)
  target_include_directories(ofexcel_tls_test PRIVATE src/include)
  target_compile_features(ofexcel_tls_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_tls_test COMMAND ofexcel_tls_test)

  # Pure: definition_hash, canonical_params, instance_key.
  add_executable(ofexcel_hashing_test test/cpp/hashing_test.cpp src/catalog/hashing.cpp src/catalog/utf8.cpp)
  target_link_libraries(ofexcel_hashing_test PRIVATE OpenSSL::Crypto)
  target_include_directories(ofexcel_hashing_test PRIVATE src/include)
  target_compile_features(ofexcel_hashing_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_hashing_test COMMAND ofexcel_hashing_test)

  # Pure: parameter and column types, canonical text values.
  add_executable(ofexcel_contract_types_test test/cpp/contract_types_test.cpp src/catalog/contract_types.cpp
                 src/catalog/utf8.cpp)
  target_include_directories(ofexcel_contract_types_test PRIVATE src/include)
  target_compile_features(ofexcel_contract_types_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_contract_types_test COMMAND ofexcel_contract_types_test)

  # Pure: the query file header and body, vectors P1–P16.
  add_executable(ofexcel_query_file_test test/cpp/query_file_test.cpp src/catalog/query_file.cpp
                 src/catalog/contract_types.cpp src/catalog/hashing.cpp src/catalog/utf8.cpp)
  target_link_libraries(ofexcel_query_file_test PRIVATE OpenSSL::Crypto)
  target_include_directories(ofexcel_query_file_test PRIVATE src/include)
  target_compile_features(ofexcel_query_file_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_query_file_test COMMAND ofexcel_query_file_test)

  # Pure: parameter values from JSON (§3.1).
  add_executable(ofexcel_param_values_test test/cpp/param_values_test.cpp src/catalog/param_values.cpp
                 src/catalog/contract_types.cpp src/catalog/hashing.cpp src/catalog/utf8.cpp)
  target_link_libraries(ofexcel_param_values_test PRIVATE OpenSSL::Crypto)
  target_include_directories(ofexcel_param_values_test PRIVATE src/include)
  target_compile_features(ofexcel_param_values_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_param_values_test COMMAND ofexcel_param_values_test)

  # Pure: Oracle column types and JSON cells, vectors T1–T26 and J1–J8.
  add_executable(ofexcel_result_types_test test/cpp/result_types_test.cpp src/catalog/result_types.cpp
                 src/catalog/contract_types.cpp src/catalog/utf8.cpp)
  target_include_directories(ofexcel_result_types_test PRIVATE src/include)
  target_compile_features(ofexcel_result_types_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_result_types_test COMMAND ofexcel_result_types_test)

  # Pure: Oracle SQL tokenizer and literals, vectors R1–R30.
  add_executable(ofexcel_oracle_render_test test/cpp/oracle_render_test.cpp src/catalog/oracle_render.cpp
                 src/catalog/param_values.cpp src/catalog/query_file.cpp src/catalog/contract_types.cpp
                 src/catalog/hashing.cpp src/catalog/utf8.cpp)
  target_link_libraries(ofexcel_oracle_render_test PRIVATE OpenSSL::Crypto)
  target_include_directories(ofexcel_oracle_render_test PRIVATE src/include)
  target_compile_features(ofexcel_oracle_render_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_oracle_render_test COMMAND ofexcel_oracle_render_test)

  # Local SQL validation over DuckDB's parser, vectors L1–L17. Opens no database.
  add_executable(ofexcel_local_validator_test test/cpp/local_validator_test.cpp src/run/local_validator.cpp)
  target_link_libraries(ofexcel_local_validator_test PRIVATE duckdb_static)
  target_include_directories(ofexcel_local_validator_test PRIVATE src/include)
  target_compile_features(ofexcel_local_validator_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_local_validator_test COMMAND ofexcel_local_validator_test)

  # DuckDB types of Local results (§3.4). Links DuckDB, opens no database.
  add_executable(ofexcel_local_types_test test/cpp/local_types_test.cpp src/run/local_types.cpp
                 src/catalog/contract_types.cpp src/catalog/utf8.cpp)
  target_link_libraries(ofexcel_local_types_test PRIVATE duckdb_static)
  target_include_directories(ofexcel_local_types_test PRIVATE src/include)
  target_compile_features(ofexcel_local_types_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_local_types_test COMMAND ofexcel_local_types_test)

  # Starts the HTTPS server inside an in-memory database and talks to it over
  # 127.0.0.1 on an ephemeral port. Its certificates come from running the
  # ofexceld binary itself: `install-cert --no-trust --dir <temporary>`.
  add_executable(ofexcel_server_test test/cpp/server_test.cpp)
  target_link_libraries(ofexcel_server_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS} httplib::httplib)
  target_include_directories(ofexcel_server_test PRIVATE src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
  target_compile_features(ofexcel_server_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_server_test COMMAND ofexcel_server_test $<TARGET_FILE:ofexceld>)

  # The local profile on a live HTTPS server: /addin/* with the session token,
  # G12–G16 against the real guard, paths that try to leave the add-in.
  add_executable(ofexcel_local_profile_test test/cpp/local_profile_test.cpp)
  target_link_libraries(ofexcel_local_profile_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS} httplib::httplib)
  target_include_directories(ofexcel_local_profile_test PRIVATE src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
  target_compile_features(ofexcel_local_profile_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_local_profile_test COMMAND ofexcel_local_profile_test)

  # Drives oracle_fusion_query through ofquack's transport seam with scripted
  # SOAP responses: pins the ofquack behaviour CONTRACTS.md §5 relies on,
  # without a Fusion instance.
  add_executable(ofexcel_fusion_seam_test test/cpp/fusion_seam_test.cpp)
  target_link_libraries(ofexcel_fusion_seam_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS})
  target_include_directories(ofexcel_fusion_seam_test PRIVATE src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES}
                             ${OFEXCEL_FUSION_SCANNER_SRC}/src)
  target_compile_features(ofexcel_fusion_seam_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_fusion_seam_test COMMAND ofexcel_fusion_seam_test)

  # The start sequence of `ofexceld serve` (src/daemon/startup.cpp), in process,
  # against a temporary QEXCEL_HOME.
  add_executable(ofexcel_daemon_test test/cpp/daemon_test.cpp ${OFEXCELD_SOURCES})
  target_link_libraries(ofexcel_daemon_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS})
  target_include_directories(ofexcel_daemon_test PRIVATE src src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
  target_compile_features(ofexcel_daemon_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_daemon_test COMMAND ofexcel_daemon_test)

  # Connections and sign-in over HTTPS (TASKS.md A4): meta.connections, the
  # secrets recreated by serve's start sequence, /test through ofquack's
  # transport seam. No browser, no Fusion.
  add_executable(ofexcel_connections_test test/cpp/connections_test.cpp ${OFEXCELD_SOURCES})
  target_link_libraries(ofexcel_connections_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS} httplib::httplib)
  target_include_directories(ofexcel_connections_test PRIVATE src src/include test/cpp
                             ${OFEXCEL_LINKED_EXTENSION_INCLUDES} ${OFEXCEL_FUSION_SCANNER_SRC}/src)
  target_compile_features(ofexcel_connections_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_connections_test COMMAND ofexcel_connections_test)

  # The query registry over HTTPS (TASKS.md C1): project files, a workbook's
  # set, saving from the editor, validation, resolving a name (§10.2).
  add_executable(ofexcel_queries_test test/cpp/queries_test.cpp ${OFEXCELD_SOURCES})
  target_link_libraries(ofexcel_queries_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS} httplib::httplib)
  target_include_directories(ofexcel_queries_test PRIVATE src src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
  target_compile_features(ofexcel_queries_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_queries_test COMMAND ofexcel_queries_test)

  # Oracle runs (TASKS.md C3) through ofquack's transport seam: types and stored
  # values, an empty result, the row limit, Oracle's refusal, cancellation, the
  # timeout, versions of an instance, the cleanup at start.
  add_executable(ofexcel_runs_test test/cpp/runs_test.cpp ${OFEXCELD_SOURCES})
  target_link_libraries(ofexcel_runs_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS})
  target_include_directories(ofexcel_runs_test PRIVATE src src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES}
                             ${OFEXCEL_FUSION_SCANNER_SRC}/src)
  target_compile_features(ofexcel_runs_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_runs_test COMMAND ofexcel_runs_test)

  # Trust (TASKS.md C2) and the run and dataset routes (TASKS.md C4) over HTTPS,
  # with ofquack's transport scripted.
  add_executable(ofexcel_runs_api_test test/cpp/runs_api_test.cpp ${OFEXCELD_SOURCES})
  target_link_libraries(ofexcel_runs_api_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS} httplib::httplib)
  target_include_directories(ofexcel_runs_api_test PRIVATE src src/include test/cpp ${OFEXCEL_LINKED_EXTENSION_INCLUDES}
                             ${OFEXCEL_FUSION_SCANNER_SRC}/src)
  target_compile_features(ofexcel_runs_api_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_runs_api_test COMMAND ofexcel_runs_api_test)

  # Local runs and their dependencies (TASKS.md C5): deps modes, meta.missing,
  # a failing dependency, Local result types, with ofquack's transport scripted.
  add_executable(ofexcel_local_runs_test test/cpp/local_runs_test.cpp ${OFEXCELD_SOURCES})
  target_link_libraries(ofexcel_local_runs_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS})
  target_include_directories(ofexcel_local_runs_test PRIVATE src src/include test/cpp
                             ${OFEXCEL_LINKED_EXTENSION_INCLUDES} ${OFEXCEL_FUSION_SCANNER_SRC}/src)
  target_compile_features(ofexcel_local_runs_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_local_runs_test COMMAND ofexcel_local_runs_test)

  # Importing a sheet's table over HTTPS (TASKS.md C7): batches, commit, expiry.
  add_executable(ofexcel_imports_test test/cpp/imports_test.cpp ${OFEXCELD_SOURCES})
  target_link_libraries(ofexcel_imports_test PRIVATE duckdb_static ${OFEXCEL_LINKED_EXTENSIONS} httplib::httplib)
  target_include_directories(ofexcel_imports_test PRIVATE src src/include test/cpp
                             ${OFEXCEL_LINKED_EXTENSION_INCLUDES})
  target_compile_features(ofexcel_imports_test PRIVATE cxx_std_17)
  add_test(NAME ofexcel_imports_test COMMAND ofexcel_imports_test)

  # `ofexceld serve` as a process: what it prints, the files it leaves, a live
  # HTTPS request. A bash script, so not on Windows.
  if(NOT WIN32)
    add_test(NAME ofexcel_serve_test
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/http/serve_test.sh $<TARGET_FILE:ofexceld>)
  endif()

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
