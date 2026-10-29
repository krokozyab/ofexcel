#!/usr/bin/env bash
# `ofexceld serve` as a process, in a temporary QEXCEL_HOME (TASKS.md A3):
#   - stdout carries exactly one line, "listening on https://127.0.0.1:<port>";
#     the start line and errors go to stderr;
#   - QEXCEL_HOME/data.duckdb is created, at schema version 1;
#   - QEXCEL_HOME/init.sql runs;
#   - the daemon answers over HTTPS and stops cleanly on SIGTERM;
#   - --db meta.duckdb is refused before anything is created.
#
#   test/http/serve_test.sh <path to ofexceld>
set -euo pipefail

OFEXCELD="$1"
HOME_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ofexcel_serve_test.XXXXXX")"
DAEMON=
cleanup() {
	if [[ -n "$DAEMON" ]]; then
		kill "$DAEMON" 2>/dev/null || true
	fi
	rm -rf "$HOME_DIR"
}
trap cleanup EXIT
export QEXCEL_HOME="$HOME_DIR/QExcel"

fail() {
	echo "serve_test: $*" >&2
	for f in "$HOME_DIR"/*.out "$HOME_DIR"/*.err; do
		[[ -f "$f" ]] && { echo "--- $f" >&2; cat "$f" >&2; }
	done
	exit 1
}

"$OFEXCELD" install-cert --no-trust > /dev/null
printf "CREATE TABLE main.from_init AS SELECT 'ran' AS marker;\n" > "$QEXCEL_HOME/init.sql"

"$OFEXCELD" serve --origin https://app.example.com --port 0 > "$HOME_DIR/serve.out" 2> "$HOME_DIR/serve.err" &
DAEMON=$!

# Wait for the listening line, at most 30 s.
for _ in $(seq 1 300); do
	grep -q "listening on" "$HOME_DIR/serve.out" && break
	kill -0 "$DAEMON" 2>/dev/null || fail "ofexceld serve exited before listening"
	sleep 0.1
done
grep -q "listening on" "$HOME_DIR/serve.out" || fail "no listening line within 30 s"

[[ "$(wc -l < "$HOME_DIR/serve.out" | tr -d ' ')" == 1 ]] || fail "stdout has more than the listening line"
grep -Eq '^listening on https://127\.0\.0\.1:[0-9]+$' "$HOME_DIR/serve.out" || fail "unexpected stdout"
URL="$(sed 's/^listening on //' "$HOME_DIR/serve.out")"
grep -q "database $QEXCEL_HOME/data.duckdb, init $QEXCEL_HOME/init.sql" "$HOME_DIR/serve.err" ||
	fail "start line on stderr does not name the database and init.sql"

STATUS="$(curl -s -o "$HOME_DIR/health.out" -w '%{http_code}' --cacert "$QEXCEL_HOME/tls/ca.crt" \
	-H 'Origin: https://app.example.com' "$URL/v1/health")"
[[ "$STATUS" == 200 ]] || fail "GET /v1/health answered $STATUS"
grep -q '"product":"qexcel"' "$HOME_DIR/health.out" || fail "unexpected /v1/health body"

kill -TERM "$DAEMON"
wait "$DAEMON" || fail "ofexceld serve exited with $? after SIGTERM"
DAEMON=
grep -q "ofexceld stopped" "$HOME_DIR/serve.err" || fail "no stop line on stderr"
[[ "$(wc -l < "$HOME_DIR/serve.out" | tr -d ' ')" == 1 ]] || fail "stdout grew after the listening line"

[[ -f "$QEXCEL_HOME/data.duckdb" ]] || fail "no data.duckdb in QEXCEL_HOME"
"$OFEXCELD" sql --db "$QEXCEL_HOME/data.duckdb" \
	"SELECT max(version) AS v, (SELECT marker FROM main.from_init) AS m FROM meta.schema_version" \
	> "$HOME_DIR/check.out"
[[ "$(tail -1 "$HOME_DIR/check.out")" == "$(printf '1\tran')" ]] || fail "schema version or init.sql marker wrong"

# A database file named like a schema: refused, nothing created, nothing on stdout.
if "$OFEXCELD" serve --db "$HOME_DIR/meta.duckdb" --port 0 > "$HOME_DIR/meta.out" 2> "$HOME_DIR/meta.err"; then
	fail "--db meta.duckdb was accepted"
fi
grep -q "E_BAD_REQUEST" "$HOME_DIR/meta.err" || fail "--db meta.duckdb refused without E_BAD_REQUEST"
[[ ! -s "$HOME_DIR/meta.out" ]] || fail "--db meta.duckdb printed to stdout"
[[ ! -e "$HOME_DIR/meta.duckdb" ]] || fail "--db meta.duckdb created the file"

echo "serve_test: all checks passed"
