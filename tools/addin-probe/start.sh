#!/usr/bin/env bash
# Starts everything the add-in probe needs (phase 0 only):
#   1. loads probe.ap_lines from Fusion into state/probe.duckdb, once (SSO sign-in);
#   2. ofexceld serve with /v1/probe/rows on http://127.0.0.1:47321;
#   3. the HTTPS dev server on https://localhost:3000.
#
#   FUSION_ENDPOINT=https://fa-xxxx.fa.ocs.oraclecloud.com \
#   FUSION_REPORT_PATH=/Custom/CloudSQL/RP_ARB.xdo \
#   tools/addin-probe/start.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OFEXCELD="$ROOT/build/release/extension/ofexcel/ofexceld"
STATE="$HERE/state"
DB="$STATE/probe.duckdb"
mkdir -p "$STATE"

if [[ ! -f "$DB" ]]; then
	: "${FUSION_ENDPOINT:?set FUSION_ENDPOINT}" "${FUSION_REPORT_PATH:?set FUSION_REPORT_PATH}"
	sed -e "s#__FUSION_ENDPOINT__#$FUSION_ENDPOINT#g" -e "s#__FUSION_REPORT_PATH__#$FUSION_REPORT_PATH#g" \
		"$HERE/load_data.sql" > "$STATE/load.sql"
	echo "Loading probe.ap_lines (SSO sign-in, then ~2 minutes)..."
	"$OFEXCELD" sql --db "$DB" --init "$STATE/load.sql" "SELECT count(*) AS rows FROM probe.ap_lines"
	rm -f "$STATE/load.sql"
fi

echo "SET ofexcel_probe_routes = true;" > "$STATE/serve_init.sql"
"$OFEXCELD" serve --db "$DB" --init "$STATE/serve_init.sql" --origin https://localhost:3000 --port 47321 &
DAEMON=$!
trap 'kill $DAEMON 2>/dev/null' EXIT
node "$HERE/server.mjs"
