#!/bin/bash
# 🎯T29 repro loop: run one dist-TSan iteration in the arm64 container,
# time it, parse CSP_PROC_HIGH_WATER, append a CSV row.
# Usage: ./t29-run-one.sh <run-id> <arm-label> [note]
set -u
RUN="$1"
ARM="$2"
NOTE="${3:-}"
LOG="t29-logs/run-${RUN}.log"
CSV="t29-repro-results.csv"

START=$(date +%s)
docker run --rm --platform linux/arm64 --cpuset-cpus=0-3 -e CSP_PROC_STATS=1 \
  -e "TSAN_OPTIONS=suppressions=test/tsan_suppressions.txt external_symbolizer_path=/usr/lib/llvm-18/bin/llvm-symbolizer" \
  -v "$PWD:/src" -w /src csp-test-linux-arm64 \
  make CC=clang-18 CXX="clang++-18 -std=c++20 -stdlib=libc++" SANITIZE=thread TEST_TIMEOUT=900 test-dist \
  > "$LOG" 2>&1
RC=$?
END=$(date +%s)
WALL=$((END - START))

# The suite spawns/shuts down the runtime many times; one stats line per
# process. Take the max across lines.
HW=$(grep -o 'CSP_PROC_HIGH_WATER=[0-9]*' "$LOG" | cut -d= -f2 | sort -n | tail -1)
HW=${HW:-NA}

if [ $RC -ne 0 ]; then
  CORES=$(ls core.* 2>/dev/null | tr '\n' ' ')
  NOTE="${NOTE:+$NOTE; }rc=$RC${CORES:+; cores: $CORES}"
fi

echo "${RUN},${ARM},${WALL},${RC},${HW},${NOTE}" >> "$CSV"
echo "RUN=${RUN} arm=${ARM} wall=${WALL}s rc=${RC} high_water=${HW} note=${NOTE}"
