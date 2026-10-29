#!/usr/bin/env bash
# Runs one phase 0 probe against a real Fusion instance with the DuckDB shell
# this repository builds. Results are recorded in docs/PHASE0_REPORT.md.
#
#   FUSION_ENDPOINT=https://fa-xxxx.fa.ocs.oraclecloud.com \
#   FUSION_REPORT_PATH=/Custom/CloudSQL/RP_ARB.xdo \
#   tools/phase0/run.sh types|throughput|locked|temp_views
#
# The first statement of each probe signs in through the browser (SSO).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROBE="$ROOT/tools/phase0/$1.sql"
[[ -f "$PROBE" ]] || { echo "no probe $1" >&2; exit 2; }
sed -e "s#__FUSION_ENDPOINT__#${FUSION_ENDPOINT:-}#g" -e "s#__FUSION_REPORT_PATH__#${FUSION_REPORT_PATH:-}#g" "$PROBE" |
	"$ROOT/build/release/duckdb" -no-agent
