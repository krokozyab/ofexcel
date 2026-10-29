# AGENTS.md

Guidance for coding agents working in this repository. Read it before changing anything.

## What this is

`ofexcel` is the backend of QExcel: a DuckDB C++ extension plus `ofexceld`, one binary that
links DuckDB, this extension, `fusion_scanner` (ofquack) and `core_functions` statically. The
Excel add-in talks to `ofexceld` over HTTP on 127.0.0.1.

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
./build/release/extension/ofexcel/ofexcel_server_test       # HTTP on an ephemeral port
./build/release/extension/ofexcel/ofexcel_fusion_seam_test  # ofquack behind a fake transport
```

Running the daemon by hand:
```sh
./build/release/extension/ofexcel/ofexceld serve --origin https://localhost:3000 --port 47321
curl -s -H 'Origin: https://localhost:3000' http://127.0.0.1:47321/v1/health
```

## Layout

```
src/ofexcel_extension.cpp   extension entry point: registers settings and functions
src/sql_functions.cpp       ofexcel_* SQL functions and settings (CONTRACTS.md §13)
src/main.cpp                ofexceld command line
src/db_util.cpp             Execute / QueryRows over DuckDB 2.0's chunked results
src/meta/migrations.cpp     meta schema, versioned (CONTRACTS.md §6.2)
src/server/request_guard.cpp  Host / Origin / session checks — pure, no DuckDB, no httplib
src/server/http_server.cpp  server lifecycle, guard, CORS, error fallbacks
src/server/routes.hpp       what a route file needs
src/server/routes_*.cpp     one file per route group
src/server/json_body.cpp    JsonObject, ErrorBody — the only way to build JSON
test/sql/                   sqllogictests
test/cpp/                   C++ suites; test_support.hpp is shared
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
- Code under `src/server/request_guard.cpp` and any future pure module must not include
  `duckdb.hpp` or `httplib.h`, so it stays testable in a second.

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

### HTTP routes
- One file per route group, `src/server/routes_<group>.cpp`; see the comment in `routes.hpp`.
- Each request opens its own connection with `RouteContext::Connect()`. Never share a
  `duckdb::Connection` between requests or threads.
- Every `/v1` response calls `SetVersionHeaders`.
- JSON is built with `JsonObject` only. Never concatenate JSON strings.

### Data and SQL
- Values from requests or Excel cells **never** go into SQL text by concatenation. DuckDB SQL takes
  them as prepared-statement parameters. Oracle SQL takes them only through the renderer that
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
- Local SQL validation (CONTRACTS.md §7);
- trust handling (CONTRACTS.md §9.3);
- the type contract (CONTRACTS.md §3);
- anything shared between HTTP threads.
