#!/usr/bin/env bash
# scripts/hunt.sh — full-suite stall/abort hunter (🎯T36 / paper 34)
#
# Runs the test binary N times. On stall (no log growth for STALL_SECS)
# samples the process (macOS `sample`) and kills it. On non-zero exit
# records the log and continues (or exits if FAIL_FAST=1).
#
# Usage:
#   scripts/hunt.sh [bin] [out_dir] [runs]
#   scripts/hunt.sh ./build/normal/csp_tests /tmp/hunt 100
#
# Env:
#   STALL_SECS   seconds of no log growth before declaring stall (default 90)
#   POLL_SECS    poll interval (default 5)
#   FAIL_FAST    if 1, exit on first non-zero (default 0 — keep hunting)
#   SAMPLE_SECS  macOS sample duration (default 5)

set -u

BIN=${1:-./build/normal/csp_tests}
OUT=${2:-/tmp/csp-hunt}
RUNS=${3:-100}
STALL_SECS=${STALL_SECS:-90}
POLL_SECS=${POLL_SECS:-5}
FAIL_FAST=${FAIL_FAST:-0}
SAMPLE_SECS=${SAMPLE_SECS:-5}

if [[ ! -x "$BIN" ]]; then
    echo "error: binary not executable: $BIN" >&2
    exit 2
fi

mkdir -p "$OUT"
summary="$OUT/summary.txt"
: >"$summary"

echo "hunt: bin=$BIN out=$OUT runs=$RUNS stall=${STALL_SECS}s" | tee -a "$summary"

fails=0
stalls=0
passes=0

for i in $(seq 1 "$RUNS"); do
    log="$OUT/run_$i.log"
    echo "=== run $i / $RUNS ===" | tee -a "$summary"

    # -d: per-case timing lines so stall detection sees progress mid-suite.
    "$BIN" -d --no-colors >"$log" 2>&1 &
    pid=$!

    stall=0
    last=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep "$POLL_SECS"
        if [[ -f "$log" ]]; then
            sz=$(wc -c <"$log" | tr -d ' ')
        else
            sz=0
        fi
        if [[ "$sz" -eq "$last" ]]; then
            stall=$((stall + POLL_SECS))
        else
            stall=0
            last=$sz
        fi
        if [[ $stall -ge $STALL_SECS ]]; then
            echo "STALL run=$i (log $sz bytes, silent ${stall}s)" | tee -a "$summary"
            if command -v sample >/dev/null 2>&1; then
                sample "$pid" "$SAMPLE_SECS" -file "$OUT/stall_$i.txt" >/dev/null 2>&1 || true
                echo "  sampled -> stall_$i.txt" | tee -a "$summary"
            fi
            # Last progressing tests for triage.
            {
                echo "--- last test lines ---"
                grep -E '^[0-9.]+ s:|TEST CASE:' "$log" | tail -20
            } | tee -a "$summary"
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            stalls=$((stalls + 1))
            fails=$((fails + 1))
            if [[ "$FAIL_FAST" == "1" ]]; then
                echo "FAIL_FAST: stopping after stall" | tee -a "$summary"
                echo "passes=$passes fails=$fails stalls=$stalls" | tee -a "$summary"
                exit 42
            fi
            continue 2
        fi
    done

    wait "$pid"
    rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "run=$i rc=$rc (non-hang failure)" | tee -a "$summary"
        {
            echo "--- tail ---"
            tail -20 "$log"
            echo "--- terminate/crash markers ---"
            grep -n 'terminating\|SIGABRT\|CRASHED\|FATAL ERROR\|Status:' "$log" | tail -20
        } | tee -a "$summary"
        cp "$log" "$OUT/fail_$i.log"
        fails=$((fails + 1))
        if [[ "$FAIL_FAST" == "1" ]]; then
            echo "FAIL_FAST: stopping after rc=$rc" | tee -a "$summary"
            echo "passes=$passes fails=$fails stalls=$stalls" | tee -a "$summary"
            exit "$rc"
        fi
    else
        passes=$((passes + 1))
        echo "run=$i OK" | tee -a "$summary"
    fi
done

echo "all $RUNS runs done: passes=$passes fails=$fails stalls=$stalls" | tee -a "$summary"
if [[ $fails -ne 0 ]]; then
    exit 1
fi
exit 0
