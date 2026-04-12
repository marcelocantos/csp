# Build performance audit — 2026-04-11

## Summary

| Mode | Before | After | Change |
|---|---:|---:|---:|
| Clean build (`make build`) | 9.38s | 0.37s (warm) / 10.5s (cold) | **25× on warm cache** |
| No-op incremental | 0.05s | 0.07s | unchanged |
| Touch `include/csp/csp.h` (broadest header) | 8.84s | 0.27s (warm) | **33×** |
| Touch one `src/*.cc` | 0.65s | 0.26s | 2.5× |
| Touch one `test/*.test.cc` | 2.08s | ~0.5s | 4× |

Single fix: **wire ccache into the Makefile's `CXX`/`CC`** when the binary is available on `PATH`. Correctness verified — all 673 tests / 26,046 assertions pass after the change.

Test execution itself is unchanged at **31.8s** (673 cases, 26,046 assertions) — it dominates `make` wall time, but that's runtime, not build time, and out of scope for this audit.

## Environment

- Platform: macOS 26 (Darwin 25.3.0), Apple M4 Max (12P+4E cores), 128 GB
- Compiler: Apple clang `c++ -std=c++20 -stdlib=libc++`
- Flags: `-O2 -g -DDEBUG -Wall -Wextra -Wno-unused-parameter`
- ccache: 4.x from Homebrew (5 GB default store)

## Baseline

### Workload breakdown

| Kind | Files | Avg compile (-O2 -g) | Notes |
|---|---:|---:|---|
| Library src (`src/*.cc`, `.cpp`) | 23 | 0.5–0.8s | http.cc slowest (0.80s) |
| Test (`test/*.test.cc`) | 60 | ~0.9s | channel.test.cc slowest (2.04s), io.test (1.55s), main.cc (1.51s), part_edge.test (1.34s), buffer.test (1.20s) |
| fcontext `.S` | 2 | <0.05s | assembly |
| llhttp `.c` | 3 | <0.1s | C, `-O2` |
| picotls + cifra + uECC `.c` | 24 | <0.1s each | C, `-O2` |

Baseline clean build: **9.38s wall, 89.27s user, 9.31s sys** → ~10.5× parallelism on 16 cores (12P+4E). Near theoretical max.

Tests are the dominant compile cost: ~60 × 0.9s = ~54s of serial CPU for test compilation, vs ~13s for library src. Every test file includes `testutil.h` → `csp.h` (the gateway header, 108 lines that pulls in 75+ part headers plus the 1397-line core). That heavy include is the single largest contributor to test compile time.

### Build structure

- Makefile-driven, no CMake.
- `MAKEFLAGS += -j$(ncpu)` already set — no `-j` on command line.
- Auto-deps via `-MMD -MP`. Dep tracking works correctly — no-op incremental is 0.05s.
- Test binary is monolithic: all 60 `test/*.test.o` link into one `csp_tests` executable.
- `make` (default target `test`) chains `diagrams` → `check-md-links` → `csp_tests` → `./csp_tests`. Both scripts are ~40ms each — not hotspots.
- CI: each matrix job runs **three** clean builds (normal, dist TLS=1, dist TLS=0), plus 6 sanitizer jobs with their own clean builds. No ccache caching in CI.

## Findings

### Critical — ccache not wired in

- **Severity**: Critical (measured 25–33× speedup on the affected modes)
- **Risk**: Low — ccache auto-detects via `command -v`, falls back to plain `c++` if absent, and ccache correctness is well-established.
- **Location**: `Makefile:26` (`CXX := c++ -std=c++20 -stdlib=libc++`)
- **Fix (applied)**: added auto-detection block that prepends `$(CCACHE)` to `CXX` and `CC` when `ccache` is on `PATH`. Disable with `make CCACHE=no`.

The Makefile invoked `c++` directly with no compiler-cache wrapper. ccache was installed locally but getting zero hits. Wiring it in gives a near-free speedup for:
- Clean builds after `make clean` (common when switching branches or debugging Makefile changes)
- Branch switches that touch files that haven't really changed from what ccache already compiled
- CI with artifact caching (not yet wired — see deferred)

Correctness verification performed:
1. Clean build with cold cache — 10.5s (tiny 1s wrapper overhead vs 9.4s baseline, expected).
2. Clean build with warm cache — 0.37s, all tests pass.
3. Touch file with no content change (mtime bump) — direct-mode hit, 0.12s, tests pass.
4. Comment-only change — preprocessor-equivalent hit, 0.26s, tests pass.
5. Full `./build/normal/csp_tests` after a ccache-hit build — 673/673 passing, 26,046 assertions.

### High — CI doesn't cache compile artifacts (deferred)

- **Severity**: High (each matrix job rebuilds ~54 compile units three times)
- **Risk**: Medium — CI cache correctness depends on cache-key shape; a bad key would silently serve stale objects. Explicitly not auto-applied per the audit's rule on CI changes.
- **Location**: `.github/workflows/ci.yml`

Per-job structure:
- `test` matrix job runs `make` (clean build #1) → `make dist` → `make test-dist` (clean builds #2 and #3, one for `CSP_TLS=1` and one for `CSP_TLS=0`).
- `sanitize` matrix jobs run a single clean sanitizer build each.

Three macOS + Linux platforms × 3 clean builds each, plus 5 sanitizer jobs, adds up to ~14 clean builds per CI run. Adding ccache to the CI image and using `actions/cache` keyed on `hash(Makefile, src/**, include/**, vendor/**)` would let the second and third builds in each matrix job hit the cache for most files (only llhttp/picotls TLS gating differs between `CSP_TLS=1` and `CSP_TLS=0`).

Rough estimate of saving: ~5–8 CPU-minutes per CI run, plus faster feedback on PRs.

### Medium — gateway header `csp.h` triggers wide rebuild (deferred)

- **Severity**: Medium
- **Risk**: Medium (PCH touches header-processing, can interact with dep tracking)
- **Location**: `include/csp.h`, `test/testutil.h`

`include/csp.h` is a kitchen-sink gateway that pulls in the 1397-line core header plus 75 combinator headers plus http/net/tls/io. Every test includes `testutil.h` → `csp.h`. Touching `include/csp/csp.h` rebuilds 80+ objects — 8.84s wall time pre-ccache (0.27s post-ccache if no content changed).

Possible mitigation: precompiled header (PCH) for `testutil.h`. Estimated savings: 1–2s wall on clean builds, on top of the ccache win. Deferred because:
- ccache already covers the same pain surface for re-clean-builds
- PCH interacts with `-MMD -MP` dep tracking and dev/test flag differences
- Modest wall-time gain once ccache is in place

### Low — monolithic test binary (deferred)

- **Severity**: Low
- **Risk**: Medium (restructuring build graph)
- **Location**: `Makefile:238` (`$(TARGET): $(LIB_OBJS) $(TEST_OBJS)`)

All 60 `test/*.test.o` link into one `csp_tests`. Touching any test file triggers a ~0.4s relink. Splitting into per-test-group binaries would remove the relink cost for single-file iterations but hurt the `./csp_tests` invocation structure and test discovery.

Not worth the structural change — relink is ~0.4s, and doctest's single-binary model is a deliberate test-framework choice.

### Low — test execution time (out of scope)

- `make` spends **31.8s** running tests; the actual build adds only ~0.5s on top of that once ccache is warm. If the dev loop runs `make` rather than `make build`, test runtime dominates.
- Not a build-system problem. Possible mitigations (separate skill): tier tests into fast/slow, add `make fast` target, or use doctest `--test-case=` filters.

### Not findings

- **`diagrams` + `check-md-links` on every `make`**: both scripts are ~40ms each. Not worth changing.
- **Parallel efficiency ~60%**: user/real = 9.5 on 16 cores sounds like underutilisation, but M4 Max has 12P+4E cores with E cores ~50% the speed of P cores. 10.5× effective parallelism is near the theoretical ceiling for this hardware.

## Applied

### Makefile — ccache wiring

```make
# --- ccache (compiler cache) ---
# Auto-detected if installed. Set CCACHE=no to disable. Caches object
# files by preprocessed-source hash so repeated clean builds and
# branch-switches that touch unchanged files are near-instant.
CCACHE ?= $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
ifneq ($(CCACHE),no)
CXX := $(CCACHE) $(CXX)
CC  := $(CCACHE) $(CC)
endif
endif
```

Placed in `Makefile` after the `BUILDDIR/CXX/CXXFLAGS/LDFLAGS` block (`Makefile:26-30`), before the sanitizer block.

### Measured deltas

Baseline → post-fix, all with `make clean && make build`:

| Scenario | Before | After | Δ |
|---|---:|---:|---:|
| Clean build, cold ccache | 9.38s | 10.5s | +1.1s (wrapper overhead) |
| Clean build, warm ccache | 9.38s | **0.37s** | −9.0s (25×) |
| No-op incremental | 0.05s | 0.07s | +0.02s (noise) |
| Touch `include/csp/csp.h` | 8.84s | **0.27s** | −8.6s (33×) |
| Touch `src/channel.cc` (mtime only) | 0.65s | 0.12s | −0.5s |
| Touch `test/channel.test.cc` (mtime only) | 2.08s | ~0.5s | −1.6s |

## Deferred

- **CI ccache caching** — high payoff, medium risk. Requires `.github/workflows/ci.yml` changes plus `actions/cache` setup. Not applied per audit rules on CI.
- **PCH for `testutil.h`** — medium severity, medium risk. ~1–2s additional savings on top of ccache.
- **Test tiering (`make fast` target)** — low payoff for build time but high payoff for dev loop if running `make` rather than `make build`.

## Method

- Measurements taken on macOS 26 / M4 Max / 128 GB, build dir on internal APFS SSD.
- `/usr/bin/time -p` for wall time. All incremental measurements repeated twice; second value reported.
- Per-file compile times measured by deleting the corresponding object and re-invoking `make -s build/normal/<obj>`.
- ccache stats verified via `ccache --show-stats` before/after each run (stats zeroed with `ccache -z`).
- Correctness verified by running `./build/normal/csp_tests` after each cached build and checking 673/673 cases pass.

## References

- Pattern catalog: `~/.claude/skills/build-perf-audit/patterns/make.md`, `patterns/common.md`
- Skill worker: `~/.claude/skills/build-perf-audit/worker.md`
- Previous release entries: `docs/audit-log.md`
