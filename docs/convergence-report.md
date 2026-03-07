# Convergence Report

_Generated: 2026-03-08 | SHA: 3776b2f_

## Standing invariants

- **Tests (local)**: PASSING — 636/636 (source), 628/628 (dist)
- **CI**: Pending — triggered run 22808939438 for 3776b2f. Previous run: Windows crashes with ACCESS_VIOLATION during "cancel with reason" test (NT_TIB StackLimit fix applied). Linux x86_64 flaky TLS SIGABRT. 8-9/11 non-Windows jobs pass.

## Movement

- 🎯T1.3: close → **close** (NT_TIB StackLimit root cause identified and fixed: make_fcontext set StackLimit to bottom of 1MB MEM_RESERVE region but only 64KB committed. Exception dispatch probed into uncommitted pages → double-fault. Fix: pass committed size to make_fcontext, update StackLimit in VEH handler and maybe_shrink. Awaiting CI confirmation.)

## Gap report

### 🎯T1 Windows port PR is merged to master  [high]
Gap: **converging** (2/3 sub-targets achieved, 1 close)

- [x] 🎯T1.1 Dist files regenerated — **achieved**
- [x] 🎯T1.2 macOS mn.test flake — **achieved**
- [ ] 🎯T1.3 Windows runtime crash — **close**: Previous .pdata fix (d668bb3) did NOT resolve the crash. New root cause: NT_TIB StackLimit pointed to bottom of 1MB MEM_RESERVE region, but only top 64KB was committed. During MSVC C++ exception dispatch, RtlVirtualUnwind probed the stack using StackLimit → hit uncommitted pages → nested ACCESS_VIOLATION → process terminated without VEH notification. Fix (3776b2f): pass committed size to make_fcontext, update StackLimit in VEH handler and maybe_shrink. CI pending.

Implied: not yet delivered (PR #4 open, CI pending)

## Recommendation

Work on: **🎯T1.3 Windows test exe runs and reports results**
Await CI run 22808939438 diagnostics. If the StackLimit fix resolves the crash, investigate remaining test failures (CountForever hang, any others).

## Suggested action

1. Check CI: `gh run list --branch worktree-windows-port --repo marcelocantos/csp --limit 1`
2. If Windows tests pass: investigate CountForever hang (likely reader-death-signal not propagating on Windows reactor).
3. If crash persists: the ACCESS_VIOLATION may involve a different mechanism — check if VEH now sees the ACCESS_VIOLATION (would confirm StackLimit was the issue even if another problem remains).

<!-- convergence-deps
evaluated: 2026-03-08T22:51:00Z
sha: 3776b2f

🎯T1:
  gap: converging
  assessment: "2/3 achieved. NT_TIB StackLimit fix applied. Awaiting CI confirmation."
  read:
    - docs/targets.md
    - .github/workflows/ci.yml
    - test/main.cc
    - src/asm/make_x86_64_ms_pe_masm.asm
    - vendor/github.com/boostorg/context/src/asm/jump_x86_64_ms_pe_masm.asm
    - CMakeLists.txt
    - src/stack_pool.cc
    - include/csp/internal/stack_pool.h
    - src/csp.cc
    - test/cancel.test.cc

🎯T1.3:
  gap: close
  assessment: "NT_TIB StackLimit root cause found and fixed. make_fcontext set StackLimit to 1MB reserved bottom instead of 64KB committed bottom. CI pending."
  read:
    - src/asm/make_x86_64_ms_pe_masm.asm
    - vendor/github.com/boostorg/context/src/asm/jump_x86_64_ms_pe_masm.asm
    - src/stack_pool.cc
    - include/csp/internal/stack_pool.h
    - src/csp.cc
    - test/cancel.test.cc
    - include/csp/fcontext.h
-->
