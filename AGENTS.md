# AGENTS.md

Guidance for coding agents working in this repository. Read it before changing anything.

## What this is

`ofexcel` is the backend of QExcel: a DuckDB C++ extension plus `ofexceld`, one binary that
links DuckDB, this extension, `fusion_scanner` (ofquack) and `core_functions` statically. The
Excel add-in talks to `ofexceld` over HTTPS on 127.0.0.1, with a certificate from a local CA
(`docs/DECISIONS.md` Р6).

- **`docs/CONTRACTS.md` is normative.** Formats, rules, error codes and test vectors. When code
  and the contract disagree, the code is wrong — or the contract is, and then you stop and ask.
- **`docs/SPEC.md` is context.** Read its block «Объём MVP» first: it says which sections are
  implemented now and which are for context only.
- Work comes as tasks in `docs/TASKS.md`. Do the task, nothing beyond it; its rules (branch, report, when to stop) are in its §0.

## Build and test

```sh
git submodule update --init --recursive
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
export GEN=ninja
# Optional: build a local ofquack checkout instead of the pinned GitHub commit.
# It must be at a commit that builds against the same DuckDB as ./duckdb.
export OFQUACK_SOURCE_DIR=$HOME/projects/ofquack-duckdb2

make release          # or: make debug
make test             # sqllogictests in test/sql through build/release/test/unittest
make test_cpp         # C++ suites in test/cpp, through ctest
```

A first build compiles vcpkg dependencies and all of DuckDB; expect it to take a while. Later
builds recompile only what changed.

Artifacts:
- `build/release/extension/ofexcel/ofexceld` — the daemon;
- `build/release/duckdb` — DuckDB shell with all three extensions linked in;
- `build/release/extension/ofexcel/ofexcel_*_test` — the C++ suites.

Single tests:
```sh
./build/release/test/unittest test/sql/ofexcel.test
./build/release/extension/ofexcel/ofexcel_guard_test        # pure, < 1 s
./build/release/extension/ofexcel/ofexcel_tls_test          # pure: local CA and certificates
./build/release/extension/ofexcel/ofexcel_hashing_test      # pure: hashes and canonical JSON
./build/release/extension/ofexcel/ofexcel_contract_types_test  # pure: types, canonical text values
./build/release/extension/ofexcel/ofexcel_query_file_test   # pure: query file header, P1–P16
./build/release/extension/ofexcel/ofexcel_param_values_test # pure: parameter values from JSON
./build/release/extension/ofexcel/ofexcel_result_types_test # pure: T1–T26, J1–J8
./build/release/extension/ofexcel/ofexcel_oracle_render_test  # pure: R1–R30, list chunks, q-literals
./build/release/extension/ofexcel/ofexcel_local_types_test  # DuckDB types of Local results
./build/release/extension/ofexcel/ofexcel_local_validator_test  # L1–L17 and more per rule
./build/release/extension/ofexcel/ofexcel_server_test build/release/extension/ofexcel/ofexceld
                                                            # HTTPS on an ephemeral port
./build/release/extension/ofexcel/ofexcel_local_profile_test  # local profile: /addin/*, session token
./build/release/extension/ofexcel/ofexcel_fusion_seam_test  # ofquack behind a fake transport
./build/release/extension/ofexcel/ofexcel_daemon_test       # serve's start sequence, in process
./build/release/extension/ofexcel/ofexcel_connections_test  # connections, secrets after restart, sign-in
./build/release/extension/ofexcel/ofexcel_queries_test      # query registry: projects, workbooks, resolution
bash test/http/serve_test.sh build/release/extension/ofexcel/ofexceld   # serve as a process
```
`make test` runs `duckdb/scripts/ci/run_tests.py`; if `python3` is unusable, run
`./build/release/test/unittest "test/*"` directly — it is the same set.

Running the daemon by hand. `install-cert` creates the local CA in `QEXCEL_HOME/tls` and trusts
it for the current user; macOS asks for your password. `--no-trust --dir DIR` skips the trust
step and writes elsewhere, which is what tests and scripts use. `serve` opens
`QEXCEL_HOME/data.duckdb` and runs `QEXCEL_HOME/init.sql` if it exists (`--db`, `--init` override
them); set the environment variable `QEXCEL_HOME` to work in a scratch directory. Its stdout is
the one line `listening on <url>`; everything else is on stderr.
```sh
./build/release/extension/ofexcel/ofexceld install-cert
./build/release/extension/ofexcel/ofexceld serve --origin https://localhost:3000 --port 47321
curl -s -H 'Origin: https://localhost:3000' https://127.0.0.1:47321/v1/health
# A connection, then sign-in: a browser window opens; finish the sign-in there (up to 300 s).
curl -s -H 'Origin: https://localhost:3000' https://127.0.0.1:47321/v1/connections \
     -d '{"name":"fusion_test","environment":"TEST","endpoint":"https://<fusion-host>","report_path":"/Custom/Financials/RP_ARB.xdo"}'
curl -s --max-time 320 -H 'Origin: https://localhost:3000' https://127.0.0.1:47321/v1/auth/login \
     -d '{"connection":"fusion_test"}'
curl -s -X POST -H 'Origin: https://localhost:3000' https://127.0.0.1:47321/v1/connections/fusion_test/test
./build/release/extension/ofexcel/ofexceld uninstall-cert   # takes the CA out of the trust store
```

## Layout

```
src/ofexcel_extension.cpp   extension entry point: registers settings and functions
src/sql_functions.cpp       ofexcel_* SQL functions and settings (CONTRACTS.md §13)
src/main.cpp                ofexceld command line
src/daemon/startup.cpp      how serve opens, initialises and locks its database (CONTRACTS.md §13)
src/db_util.cpp             Execute / QueryRows over DuckDB 2.0's chunked results
src/paths.cpp               QEXCEL_HOME and the paths under it — pure
src/catalog/hashing.cpp     definition_hash, canonical_params, instance_key (CONTRACTS.md §1.1–1.3) — pure
src/catalog/contract_types.cpp  parameter / column types, canonical text values (§2.2, §3.1) — pure
src/catalog/query_file.cpp  query file header and body (§2) — pure
src/catalog/param_values.cpp  parameter values from JSON, E_PARAM_* (§3.1) — pure
src/catalog/result_types.cpp  Oracle column types, stored values, JSON cells (§3.2–3.3) — pure
src/catalog/oracle_render.cpp  Oracle SQL tokenizer and parameter literals (§4) — pure
src/run/local_types.cpp     DuckDB type → contract type for Local results (§3.4) — knows DuckDB
src/run/local_validator.cpp Local SQL validation V1–V9 over duckdb::Parser (§7)
src/run/fusion_errors.cpp   ofquack's errors → E_FUSION_* and the rest of §14.1
src/catalog/utf8.cpp        UTF-8 validation — pure
src/meta/migrations.cpp     meta schema, versioned (CONTRACTS.md §6.2)
src/meta/connections.cpp    meta.connections and the oracle_fusion secrets beside them (§8.3, §13)
src/meta/query_registry.cpp meta.query_definitions: projects, workbooks, examples; resolution (§10.2)
src/security/local_ca.cpp   local CA and server certificate (CONTRACTS.md §8.1) — pure, OpenSSL only
src/security/trust_store.cpp  CA into / out of the user's trusted roots (macOS keychain, Windows ROOT)
src/server/request_guard.cpp  Host / Origin / session checks — pure, no DuckDB, no httplib
src/server/http_server.cpp  HTTPS server lifecycle, guard, CORS, error fallbacks
src/server/routes.hpp       what a route file needs
src/server/routes_*.cpp     one file per route group; routes_addin.cpp serves /addin/* (local profile)
src/server/routes_queries.cpp  /v1/queries*, /v1/workbooks/{id}/queries
src/server/json_body.cpp    JsonObject, ErrorBody — the only way to build JSON; JsonDocument reads bodies
src/include/ofexcel/api_error.hpp  ApiError: a §14 code and status, thrown by code routes and SQL share
test/sql/                   sqllogictests
test/cpp/                   C++ suites; test_support.hpp is shared
test/http/                  scripts that run ofexceld as a process (POSIX only)
tools/live_probe.cpp        ofexcel_live_probe: checks against a real Fusion (not in ctest)
tools/phase0/               phase 0 probe scripts; results in docs/PHASE0_REPORT.md
docs/                       SPEC.md, CONTRACTS.md, DECISIONS.md, TASKS.md, PHASE0_REPORT.md
```

## Rules

### Code
- C++17, set **per target** with `target_compile_features`. Never set `CMAKE_CXX_STANDARD`:
  DuckDB owns it.
- Follow DuckDB's style: tabs, `PascalCase` functions and types, `snake_case` variables,
  `UPPER_CASE` constants. Match the comment density of the surrounding code: comments say why,
  not what.
- DuckDB 2.0 results are read chunk by chunk (`Fetch()`); there is no `MaterializedQueryResult`.
  Use `QueryRows` (metadata-sized results) or read chunks yourself.
- Code under `src/server/request_guard.cpp`, `src/security/`, `src/paths.cpp`, `src/catalog/`
  and any future pure module must not include `duckdb.hpp` or `httplib.h`, so it stays testable in a second.
- Paths are UTF-8 `std::string`; open files through `std::filesystem::u8path`. A Windows user
  name can be Cyrillic, and a narrow path would go through the ANSI code page.
- `<wincrypt.h>` and OpenSSL headers never meet in one file: wincrypt's macros (`X509_NAME`…)
  break OpenSSL's types. `trust_store.cpp` takes DER bytes from `local_ca.cpp` for that reason.

### Errors
- SQL-facing errors are DuckDB exceptions whose message starts with the code from
  CONTRACTS.md §14: `throw InvalidInputException("E_PARAM_TYPE: …")`.
- HTTP errors go through `SendError(response, status, code, message)`. The status and code come
  from CONTRACTS.md §14.
- User-facing messages are in **Russian**; log lines and comments in English.

### SQL functions
- Bind declares columns and validates arguments. The action — starting a server, running a
  query — happens in the global init, which runs when the query executes. Bind also runs for
  `EXPLAIN` and prepared statements. `sql_functions.cpp` shows the pattern.
- A new `ofexcel_*` setting goes through the `add` helper in `RegisterSettings`, which makes it
  `GLOBAL`. DuckDB 2.0's default for extension options is the session, and a setting from
  `init.sql` would then never reach the connections HTTP requests open.
- Nothing may `SET` after start: `serve` ends its setup with `lock_configuration = true`. A route
  that needs a per-request value takes it as a parameter, not as a setting.

### HTTP routes
- One file per route group, `src/server/routes_<group>.cpp`; see the comment in `routes.hpp`.
- Each request opens its own connection with `RouteContext::Connect()`. Never share a
  `duckdb::Connection` between requests or threads.
- Every `/v1` response calls `SetVersionHeaders`.
- JSON is built with `JsonObject` only. Never concatenate JSON strings. Request bodies are read
  with `JsonDocument`.
- A refusal is `throw ApiError(code, status, message)`; the route's wrapper, or the server's
  exception handler, sends it as an error body.

### Data and SQL
- Values from requests or Excel cells **never** go into SQL text by concatenation. DuckDB SQL takes
  them as prepared-statement parameters (`QueryRows(connection, sql, {values})`); a secret is
  created through `SecretManager`, not by writing `CREATE SECRET` text.
- Secrets are never `PERSISTENT`: ofexceld opens its database with `allow_persistent_secrets =
  false`. Oracle SQL takes them only through the renderer that
  CONTRACTS.md §4 specifies, with its vectors as tests.
- A schema change is a new migration version in `migrations.cpp`. Never edit a version that has
  shipped: databases already at it would not see the change.

### Tests
- Every vector in CONTRACTS.md becomes an automated test, named by its id (`R12`, `T5`, `G7`…).
- Where a test goes:
  - pure logic → `test/cpp/<module>_test.cpp`, no DuckDB;
  - SQL surface → `test/sql/*.test`;
  - HTTP → `test/cpp/server_test.cpp` or a new suite in the same style;
  - anything that needs a Fusion response → a fake transport, as in `fusion_seam_test.cpp`.
- No test reaches a real Fusion instance or the network beyond 127.0.0.1.
- Never weaken or delete a test to make a change pass. If a contract vector looks wrong, stop and ask.

## Do not
- Change anything in `duckdb/`, `extension-ci-tools/` or ofquack's sources.
- Move the DuckDB pin, the ci-tools pin or the fusion_scanner `GIT_TAG`. They move together, and
  only when asked.
- Add a dependency (vcpkg port, vendored code) without asking.
- Relax the Host / Origin / session checks, the CORS rules, or Local SQL validation.
- Serve plain HTTP, turn off certificate verification in a test client, or loosen the CA's name
  constraints.
- Implement anything SPEC.md lists as context only (package GL, server mode, phase 2–3).

## Stop and ask when
- A contract rule is ambiguous, contradicts another, or contradicts SPEC.md.
- Something marked **[Ф0]** in CONTRACTS.md turns out to be false.
- The task needs a new HTTP route, SQL function, setting, error code or table not in CONTRACTS.md.
- The task cannot be done without something outside the MVP scope.

## Changes that need review by a maintainer
Mark these in the task report; do not consider them done until reviewed:
- the Oracle SQL tokenizer and parameter rendering (CONTRACTS.md §4);
- Host / Origin / session / CORS handling;
- TLS: the local CA, certificates, trust-store code (`src/security/`);
- Local SQL validation (CONTRACTS.md §7);
- trust handling (CONTRACTS.md §9.3);
- the type contract (CONTRACTS.md §3);
- anything shared between HTTP threads.
