#!/usr/bin/env python3
"""Stack-analysis corpus metric + both-directions ratchet (🎯T52.2).

Runs the real workload corpus — the full test-suite binary and the finite
examples — under an ANALYSE build with CSP_STACK_STATS=1, aggregates the
per-process counters the runtime dumps at exit
(CSP_STACK_SPAWNS_TOTAL / _SMALL / _BYTES_SAVED), and compares the result
against a checked-in per-platform baseline.

Gated quantities, each a BOTH-directions window:
  - small_spawns: the absolute number of spawns that landed a Small slot
    (integer, stable run to run — the primary ratchet).
  - small_rate: small_spawns / total_spawns, with a looser tolerance that
    absorbs corpus-size drift as tests are added or removed.

Both directions means a value below the window fails (regression) and a
value above it also fails (an improvement must be acknowledged by a
deliberate baseline edit — run with --update-baseline and commit the
diff). This prevents silent drift either way.

Platforms without a baseline entry fail unless --allow-missing-baseline
is given (used to bring a new platform online: run once, inspect, then
--update-baseline).

Invoked by `make ANALYSE=1 stack-metric`; the skip list for non-finite
examples is inherited from the Makefile's EXAMPLE_CI_BINS.
"""

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

STATS_RE = re.compile(
    r"CSP_STACK_SPAWNS_TOTAL=(\d+) CSP_STACK_SPAWNS_SMALL=(\d+) "
    r"CSP_STACK_BYTES_SAVED=(\d+)"
)

TEST_TIMEOUT_S = 1800
EXAMPLE_TIMEOUT_S = 60


def platform_key() -> str:
    machine = platform.machine().lower()
    if machine == "aarch64":
        machine = "arm64"
    system = platform.system().lower()  # darwin, linux, windows
    return f"{system}-{machine}"


def run_corpus_binary(path: Path, timeout_s: int) -> tuple[int, int, int]:
    """Run one corpus binary; return its (total, small, bytes_saved)."""
    env = dict(os.environ, CSP_STACK_STATS="1")
    try:
        proc = subprocess.run(
            [str(path)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_s,
            text=True,
            errors="replace",
        )
    except subprocess.TimeoutExpired:
        raise SystemExit(
            f"stack-metric: {path.name} exceeded {timeout_s}s") from None
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout[-4000:])
        sys.stderr.write(proc.stderr[-4000:])
        raise SystemExit(f"stack-metric: {path.name} exited {proc.returncode}")
    m = None
    for m in STATS_RE.finditer(proc.stderr):
        pass  # keep the last match (the atexit line)
    if m is None:
        raise SystemExit(
            f"stack-metric: {path.name} printed no CSP_STACK_ stats line — "
            "is it an ANALYSE (-DCSP_ANALYSE_STACKS) build?"
        )
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def check_window(name: str, value: float, lo: float, hi: float) -> None:
    if value < lo:
        raise SystemExit(
            f"stack-metric: REGRESSION — {name} {value:g} below baseline "
            f"window [{lo:g}, {hi:g}]. Fewer spawns are landing Small slots "
            "than the recorded state.")
    if value > hi:
        raise SystemExit(
            f"stack-metric: UNACKNOWLEDGED IMPROVEMENT — {name} {value:g} "
            f"above baseline window [{lo:g}, {hi:g}]. The ratchet moves only "
            "by deliberate commit: rerun with --update-baseline and commit "
            "the diff.")
    print(f"stack-metric: OK — {name} {value:g} within [{lo:g}, {hi:g}]")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--test-bin", required=True, type=Path,
                    help="ANALYSE-build test-suite binary")
    ap.add_argument("--examples", nargs="*", type=Path, default=[],
                    help="finite ANALYSE-build example binaries")
    ap.add_argument("--baseline", required=True, type=Path,
                    help="baseline JSON (checked in)")
    ap.add_argument("--update-baseline", action="store_true",
                    help="record the measured numbers as the new baseline "
                         "for this platform (deliberate ratchet move)")
    ap.add_argument("--allow-missing-baseline", action="store_true",
                    help="emit numbers without gating when this platform "
                         "has no baseline entry")
    args = ap.parse_args()

    total = small = saved = 0
    print(f"stack-metric: running corpus ({1 + len(args.examples)} binaries)")
    for path, timeout_s in [(args.test_bin, TEST_TIMEOUT_S)] + [
        (p, EXAMPLE_TIMEOUT_S) for p in args.examples
    ]:
        t, s, b = run_corpus_binary(path, timeout_s)
        print(f"stack-metric:   {path.name}: total={t} small={s} saved={b}")
        total += t
        small += s
        saved += b

    if total == 0:
        raise SystemExit("stack-metric: corpus spawned no imps")
    rate = small / total

    key = platform_key()
    print(f"STACK_METRIC platform={key} total_spawns={total} "
          f"small_spawns={small} small_rate={rate:.9f} bytes_saved={saved}")

    baseline_all = json.loads(args.baseline.read_text())

    if args.update_baseline:
        prev = baseline_all.get(key, {})
        baseline_all[key] = {
            "small_spawns": small,
            "small_spawns_tolerance": prev.get("small_spawns_tolerance", 0),
            "small_rate": round(rate, 9),
            "rate_tolerance": prev.get("rate_tolerance", 5e-7),
            "observed": {
                "total_spawns": total,
                "bytes_saved": saved,
            },
        }
        args.baseline.write_text(
            json.dumps(baseline_all, indent=2, sort_keys=True) + "\n")
        print(f"stack-metric: baseline for {key} updated in {args.baseline}")
        return 0

    entry = baseline_all.get(key)
    if entry is None:
        msg = (f"stack-metric: no baseline entry for platform '{key}' in "
               f"{args.baseline}")
        if args.allow_missing_baseline:
            print(f"{msg} — emitting without gating "
                  "(--allow-missing-baseline)")
            return 0
        raise SystemExit(
            f"{msg}. Run with --update-baseline to record one, inspect the "
            "diff, and commit it.")

    n_tol = entry["small_spawns_tolerance"]
    check_window("small_spawns", small,
                 entry["small_spawns"] - n_tol, entry["small_spawns"] + n_tol)
    r_tol = entry["rate_tolerance"]
    check_window("small_rate", rate,
                 entry["small_rate"] - r_tol, entry["small_rate"] + r_tol)
    return 0


if __name__ == "__main__":
    sys.exit(main())
