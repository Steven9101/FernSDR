#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Every report of a session round (harness/links.py), each writing its
# summary-*.md/json next to the raw data. Low priority so that it can run
# beside a measurement without taking its cores.
#
#   bench/analysis/session.sh ROUND [ORACLE_JSON]
set -euo pipefail
here=$(dirname "$(readlink -f "$0")")
root=${1:?round directory}
oracle=${2:-}
run() { nice -n 19 prlimit --fsize=268435456 python3 "$here/$1" "${@:2}"; }
for r in listen_report links_report compression quality resources_report; do
  echo "== $r"
  run "$r.py" "$root"
done
if [ -n "$oracle" ]; then
  echo "== digital_report"
  run digital_report.py "$root" "$oracle"
fi
echo "== overview"
run overview.py "$root"
