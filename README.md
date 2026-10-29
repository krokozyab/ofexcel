# ofexcel

Backend of **QExcel** — Oracle Fusion queries in Excel: Oracle SQL to fetch data, DuckDB SQL to
analyse it locally, parameters from cells, refreshable tables.

This repository builds:
- **`ofexcel`** — a DuckDB extension with the HTTP API the Excel add-in uses;
- **`ofexceld`** — one binary with DuckDB, `ofexcel`, [`fusion_scanner`](https://github.com/krokozyab/ofquack)
  and `core_functions` linked in statically.

Status: skeleton. `ofexceld install-cert` creates a local CA and trusts it for the current user;
`ofexceld serve` then serves HTTPS on 127.0.0.1 and answers `GET /v1/health`. Everything else is
described in `docs/` and not built yet.

- `docs/SPEC.md` — architecture and the MVP scope.
- `docs/CONTRACTS.md` — formats, rules and test vectors the implementation must meet.
- `AGENTS.md` — how to build, test and change the code.

```sh
git submodule update --init --recursive
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja
make release && make test && make test_cpp
./build/release/extension/ofexcel/ofexceld install-cert      # asks for your password to trust the CA
./build/release/extension/ofexcel/ofexceld serve --origin https://localhost:3000
```
