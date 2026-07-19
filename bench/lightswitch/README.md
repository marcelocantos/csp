# Minimal-save context switch prototype (🎯T35.1)

Measures a redesigned switch *contract*: the assembly saves only
fp/lr/PC, and the call site declares x19–x28 plus the full vector file
as clobbers, so the compiler spills exactly the registers that are
live. Prototype numbers on M4 Max (2026-07-19): 9.4 ns round-trip vs
22.3 ns for Boost fcontext — 2.4×. See paper 33, round 3.

Not wired into the build; arm64-only. Build by hand:

    c++ -O2 -c light_jump.S && c++ -std=c++20 -O2 lightbench.cc light_jump.o && ./a.out
