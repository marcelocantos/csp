# Minimal-save context switch prototype (🎯T35.1)

Measures a redesigned switch *contract*: the assembly saves only
fp/lr/PC, and the call site declares x19–x28 plus the full vector file
as clobbers, so the compiler spills exactly the registers that are
live. Prototype numbers on M4 Max (2026-07-19): 9.4 ns round-trip vs
22.3 ns for Boost fcontext — 2.4×. See paper 33, round 3.

Not wired into the build; arm64-only. Build by hand:

    c++ -O2 -c light_jump.S && c++ -std=c++20 -O2 lightbench.cc light_jump.o && ./a.out

## Register-pressure comparison (switchcmp.cc)

Is an in-between strategy — asm saves a "commonly live" subset
(fp/lr + x19–x24), call site clobbers the rest — worth it? Measured
(ns per round-trip, LIVE = uint64 accumulators held live across every
switch):

| LIVE | Boost | full-clobber | in-between |
|---:|---:|---:|---:|
| 0  | 22.8 | 9.6  | 13.6 |
| 4  | 22.9 | 8.9  | 13.9 |
| 8  | 22.8 | 8.9  | 18.7 |
| 12 | 25.4 | 18.4 | 19.6 |

The in-between is dominated everywhere: it pays its subset's save
cost unconditionally, while the clobber contract already *is* the
adaptive version of "save what's commonly used" — the compiler saves
exactly the live subset per call site, and its spill code schedules
better than the asm's serial store chain. Full-clobber wins at every
pressure level, including 12 live values.

Build:  c++ -O2 -c light_jump.S light_mid.S
        c++ -std=c++20 -O2 -DVARIANT={0|1|2} -DLIVE={0|4|8|12} switchcmp.cc *.o <fcontext objs>

## Production wiring (🎯T35.1, achieved 2026-07-19)

The full-clobber contract shipped: `make LIGHT_SWITCH=1` (own build
dir `build/normal-light`). Gate: `CSP_USE_LIGHT_SWITCH` in
`csp/fcontext.h` — active on arm64 non-Windows non-sanitizer builds;
Boost fcontext everywhere else. Assembly: `src/light_switch_arm64_
{macho,elf}.S`; dist amalgamation emits it under the same gate
(`-DCSP_LIGHT_SWITCH` on the dist build enables it).

Realized (M4 Max, same epoch): yield 71.3 → 49.1 ns (−31%); pingpong
173.8 → 145.4 ns (−16%), flat across 2–16 procs.
