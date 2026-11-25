#!/usr/bin/env bash
# End-to-end UART upload check on native_sim: paste planoszc-compiled IR into the device shell,
# run it, and prove the device (a) accepts and overlaps an honest plan, (b) rejects corrupted
# uploads, (c) still rejects plans whose IR lies about WCET / resources, then replay the console log.
#
#   tools/ci/native_sim_shell.sh [path/to/zephyr.exe]      (build: make native-sim-shell)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="${1:-$ROOT/build/native_sim_shell/zephyr/zephyr.exe}"
PLANOSZC="${PLANOSZC:-planoszc}"
M="$ROOT/examples/missions"
OUT="${OUT_DIR:-$(mktemp -d)}"
mkdir -p "$OUT"
LOG="$OUT/console.log"
DELAY="${LINE_DELAY:-0.15}"

[ -x "$EXE" ] || { echo "missing $EXE (run: make native-sim-shell)"; exit 2; }

"$PLANOSZC" compile "$M/04_parallel_overlap_accept.yaml" --emit uart > "$OUT/good.txt"
"$PLANOSZC" compile "$M/04_parallel_overlap_accept.yaml" --emit uart | python3 -c '
import sys
lines = sys.stdin.read().splitlines()
i = next(k for k, l in enumerate(lines) if l.startswith("planosz load data "))
l = lines[i]
pos = len("planosz load data ") + 12
lines[i] = l[:pos] + ("B" if l[pos] != "B" else "C") + l[pos + 1:]   # flip one payload char
print("\n".join(lines))' > "$OUT/corrupt.txt"
# Wire IR carries args/deps/deadline, not resource masks — the device re-derives masks.
# understate-wcet forges the host WCET field; strip-masks only matters for C-header IR tests.
"$PLANOSZC" compile "$M/02_deadline_reject.yaml" --emit uart --force --forge understate-wcet > "$OUT/forged_deadline.txt"
"$PLANOSZC" compile "$M/03_resource_conflict_reject.yaml" --emit uart --force --forge understate-wcet > "$OUT/forged_conflict.txt"

feed() { while IFS= read -r l; do echo "$l"; sleep "$DELAY"; done < "$1"; }

{
	sleep 1
	echo "planosz status"; sleep 0.3
	echo "planosz run uploaded"; sleep 0.3                    # empty slot: must not run anything
	feed "$OUT/good.txt";           echo "planosz run uploaded"; sleep 2
	feed "$OUT/corrupt.txt";        echo "planosz run uploaded"; sleep 0.5
	feed "$OUT/forged_deadline.txt"; echo "planosz run uploaded"; sleep 0.5
	feed "$OUT/forged_conflict.txt"; echo "planosz run uploaded"; sleep 0.5
	echo "planosz run parallel"; sleep 2
	echo "sim_exit"; sleep 1
} | timeout 120 "$EXE" > "$LOG" 2>&1 || true

sed 's/\x1b\[[0-9;]*m//g' "$LOG" | tr -d '\r' > "$OUT/clean.log"
cp "$OUT/clean.log" "$LOG"

fail() { echo "FAIL: $*"; echo "---- console log ----"; cat "$LOG"; exit 1; }
need() { grep -qF -- "$1" "$LOG" || fail "expected in log: $1"; }

need '"type":"load","t_ms":0,"plan":"demo.parallel_overlap","status":"OK"'
need '"type":"load","t_ms":0,"plan":"-","status":"REJECT","code":"REJECT_UNSUPPORTED_IR"'
need '"plan":"demo.parallel_overlap","status":"ACCEPT"'
need '"plan":"demo.deadline_reject","status":"REJECT","code":"REJECT_DEADLINE_INFEASIBLE"'
need '"plan":"demo.resource_conflict","status":"REJECT","code":"REJECT_RESOURCE_CONFLICT"'
need '"ir_wcet_mismatch"'
n_ok=$(grep -c '"type":"run_end".*"status":"OK"' "$LOG" || true)
[ "$n_ok" -eq 2 ] || fail "expected exactly 2 OK runs (uploaded + registered parallel), got $n_ok"

# a corrupted upload must leave the slot empty: nothing is admitted or run until the next good load
python3 - "$LOG" <<'PY'
import sys
after_reject = False
for line in open(sys.argv[1]):
    if '"type":"load"' in line:
        after_reject = "REJECT_UNSUPPORTED_IR" in line
    elif after_reject and ('"type":"admit"' in line or '"type":"run_start"' in line):
        sys.exit("a plan was admitted/run after a rejected upload")
PY

"$PLANOSZC" replay "$LOG" --extract --tolerance-ms 5 \
	--plan "$M/02_deadline_reject.yaml" --plan "$M/03_resource_conflict_reject.yaml" \
	--plan "$M/04_parallel_overlap_accept.yaml" --require-overlap demo.parallel_overlap=2
echo "native_sim shell upload check: PASS ($OUT)"
