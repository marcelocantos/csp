#include "csp.h"

#include <doctest/doctest.h>

#if defined(__aarch64__)

// Detect sanitizers at compile time.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define CSP_SANITIZED 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#  undef CSP_SANITIZED
#  define CSP_SANITIZED 1
#endif
#ifndef CSP_SANITIZED
#  define CSP_SANITIZED 0
#endif

// Under sanitizers, instrumented code contains BL calls into ASan/TSan
// runtime (shadow memory checks, etc.) that can lead the instruction
// walker into unmapped memory.  Skip the entire suite — the analyzer
// is already bypassed at runtime (see spawn()).
#if !CSP_SANITIZED

// Prevent inlining so each function gets its own stack frame.
// At -O2, the compiler allocates minimal frames, so we check relative
// relationships rather than absolute buffer sizes.

__attribute__((noinline)) static void leaf_func(void*) {
    volatile char buf[64];
    buf[0] = 1;
}

__attribute__((noinline)) static void deeper_leaf(void*) {
    volatile char buf[64];
    // Touch multiple elements to encourage larger frame allocation.
    for (int i = 0; i < 64; ++i)
        buf[i] = static_cast<char>(i);
}

__attribute__((noinline)) static void calls_leaf(void*) {
    volatile char buf[64];
    buf[0] = 1;
    leaf_func(nullptr);
}

// A function that calls through a function pointer stored in its data arg.
struct indirect_data {
    void (*fn)(void*);
};

__attribute__((noinline)) static void indirect_caller(void* data) {
    volatile char buf[64];
    buf[0] = 1;
    auto* d = static_cast<indirect_data*>(data);
    d->fn(nullptr);
}

TEST_SUITE("StackAnalysis") {

TEST_CASE("Leaf-function") {
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    CHECK(result.is_exact);
    // Compiler allocates at least some frame for the volatile write.
    CHECK(result.max_depth > 0);
    // Should be well under 4KB for a trivial leaf.
    CHECK(result.max_depth < 4096);
    MESSAGE("leaf_func depth: ", result.max_depth);
}

TEST_CASE("Direct-call-chain") {
    // calls_leaf may tail-call into leaf_func (B instead of BL at -O2).
    // Either way, the total depth should be >= leaf_func's depth.
    auto leaf_result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    auto chain_result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&calls_leaf));
    CHECK(chain_result.is_exact);
    CHECK(chain_result.max_depth >= leaf_result.max_depth);
    MESSAGE("calls_leaf depth: ", chain_result.max_depth,
            ", leaf_func depth: ", leaf_result.max_depth);
}

TEST_CASE("Indirect-call---no-data") {
    csp::stack_analysis_options opts;
    opts.max_instructions = 10000;
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), nullptr, opts);
    // Without data, can't resolve the indirect call.
    CHECK_FALSE(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("indirect_caller (no data) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

TEST_CASE("Indirect-call---with-data") {
    indirect_data d{&leaf_func};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d);
    // With data, the indirect target should be resolved.
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("indirect_caller (with data) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

TEST_CASE("Cache-consistency") {
    auto r1 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    auto r2 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&leaf_func));
    CHECK(r1.max_depth == r2.max_depth);
    CHECK(r1.is_exact == r2.is_exact);
}

TEST_CASE("Different-indirect-targets---different-depths") {
    indirect_data d1{&leaf_func};
    indirect_data d2{&deeper_leaf};
    auto r1 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d1);
    auto r2 = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&indirect_caller), &d2);
    // Same cached program, different data → potentially different depths.
    CHECK(r1.is_exact);
    CHECK(r2.is_exact);
    // Both should produce reasonable results.
    CHECK(r1.max_depth > 0);
    CHECK(r2.max_depth > 0);
    MESSAGE("indirect w/ leaf_func: ", r1.max_depth,
            ", w/ deeper_leaf: ", r2.max_depth);
}

// ---- New tests for T3.4 improvements ----

// A virtual-dispatch-style scenario: object pointer + vtable lookup.
// The struct stores a function pointer at a known offset (simulating a vtable).
struct vtable_obj {
    void (*method)(void*);  // offset 0 — simulates first vtable slot
};

__attribute__((noinline)) static void vtable_caller(void* data) {
    volatile char buf[64];
    buf[0] = 1;
    auto* obj = static_cast<vtable_obj*>(data);
    obj->method(nullptr);  // BLR via function pointer from struct
}

TEST_CASE("Vtable-like-indirect-call---no-data") {
    // Without data the indirect call falls back to budget.
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&vtable_caller), nullptr);
    CHECK_FALSE(result.is_exact);
}

TEST_CASE("Vtable-like-indirect-call---with-data") {
    vtable_obj obj{&leaf_func};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&vtable_caller), &obj);
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("vtable_caller (with data) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// Interprocedural data forwarding: A calls B, passing a sub-pointer from
// the closure. B calls through a function pointer inside the sub-object.
struct inner_data {
    void (*callback)(void*);
};

struct outer_data {
    int tag;
    inner_data* inner;  // sub-pointer at offset 8
};

__attribute__((noinline, optnone))
static void inner_caller(void* data) {
    volatile char buf[128];
    buf[0] = 1;
    auto* d = static_cast<inner_data*>(data);
    d->callback(nullptr);
}

__attribute__((noinline))
static void outer_caller(void* data) {
    volatile char buf[64];
    buf[0] = 1;
    auto* d = static_cast<outer_data*>(data);
    inner_caller(d->inner);  // passes sub-pointer — interprocedural forwarding
}

TEST_CASE("Interprocedural-data-forwarding---no-data") {
    // Without data, both levels fall back to budget.
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&outer_caller), nullptr);
    CHECK_FALSE(result.is_exact);
}

TEST_CASE("Interprocedural-data-forwarding---with-data") {
    inner_data inner{&leaf_func};
    outer_data outer{0, &inner};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&outer_caller), &outer);
    // With forwarding, the inner indirect call should be resolved.
    // At minimum the result should be non-zero and not crash.
    CHECK(result.max_depth > 0);
    MESSAGE("outer_caller depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// Branch pruning via CBZ/CBNZ: a function with two paths of very different
// stack depth, dispatched on a boolean captured in the closure.
struct tag_data {
    int which;  // 0 = small path, 1 = large path
};

__attribute__((noinline))
static void small_path(void*) {
    volatile char buf[64];
    buf[0] = 1;
}

__attribute__((noinline))
static void large_path(void*) {
    volatile char buf[512];
    for (int i = 0; i < 512; ++i) buf[i] = static_cast<char>(i);
}

// This function is intentionally written to use a conditional branch
// that the analyzer may or may not be able to prune. We don't assert
// pruning (it requires the compiler to use CBZ/CBNZ on the loaded tag),
// but we do assert correctness in both data cases.
__attribute__((noinline))
static void branching_caller(void* data) {
    volatile char buf[32];
    buf[0] = 1;
    auto* d = static_cast<tag_data*>(data);
    if (d->which == 0) {
        small_path(nullptr);
    } else {
        large_path(nullptr);
    }
}

TEST_CASE("Branch-with-data---small-path") {
    tag_data d{0};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&branching_caller), &d);
    // With data, the CBZ on d->which is folded at walk time: only the
    // small_path branch is explored, giving an exact result.
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    // Small path is well under 512 bytes (it only has a tiny local buffer).
    CHECK(result.max_depth < 512);
    MESSAGE("branching_caller (small) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

TEST_CASE("Branch-with-data---large-path") {
    tag_data d{1};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&branching_caller), &d);
    // The large path has a loop (stack canary check) that may exceed the
    // instruction budget, so is_exact may be false — but the result should
    // be at least as large as the small path's exact result.
    CHECK(result.max_depth > 0);
    tag_data d_small{0};
    auto small_result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&branching_caller), &d_small);
    CHECK(result.max_depth >= small_result.max_depth);
    MESSAGE("branching_caller (large) depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// ---- 🎯T3.4.2: PC-relative dispatch + vtables ----

// Case (a): static const function-pointer table dispatched via ADRP+LDR+BLR.
// The table lives in __DATA_CONST,__const; the walker should follow the
// PC_RELATIVE register through the LDR, dereference once at the BLR, and
// emit a direct call to the resolved target.
//
// Using an inline asm trampoline isn't necessary — the natural C codegen
// for `(*pc_relative_table[idx])(arg)` already produces the ADRP+ADD+LDR+BLR
// sequence the walker now resolves.
__attribute__((noinline)) static void table_target_a(void*) {
    volatile char buf[64];
    buf[0] = 1;
}

// Index 0 of the table — the walker only sees a compile-time-constant
// LDR offset, so resolution is deterministic.
static void (*const pc_relative_table[])(void*) = {
    &table_target_a,
};

__attribute__((noinline)) static void pc_table_caller(void* arg) {
    volatile char buf[80];
    buf[0] = 1;
    pc_relative_table[0](arg);
}

TEST_CASE("PC-relative-fn-ptr-table-resolves-exact") {
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&pc_table_caller));
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("pc_table_caller depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// Case (b): virtual call through a closure-held object pointer.
// The closure holds a function-pointer-as-first-field — codegen reads
// closure[0] then BLRs through it. With data, the LDR-from-DATA_OFFSET=0
// path reads `*(void**)data` at walk time; if that lands in __TEXT the
// walker promotes to PC_RELATIVE and the BLR emits a direct call.
struct vptr_closure {
    void (*method)(void*);  // offset 0 — emulates vtable slot 0
};

__attribute__((noinline)) static void vptr_target_b(void*) {
    volatile char buf[48];
    buf[0] = 1;
}

__attribute__((noinline)) static void vptr_caller(void* data) {
    volatile char buf[72];
    buf[0] = 1;
    auto* c = static_cast<vptr_closure*>(data);
    c->method(nullptr);
}

TEST_CASE("Vtable-via-closure-resolves-exact") {
    vptr_closure c{&vptr_target_b};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&vptr_caller), &c);
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("vptr_caller depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// ---- 🎯T3.4.3: wider value loads + X0–X7 forwarding ----

// Case (R-TAG8): uint8_t discriminator branching to two paths of different
// stack depth. Before T3.4.3 the LDRB went to UNKNOWN and the analyser had
// to take both paths; with byte-load CONST materialisation, the CBZ folds
// at walk time and the result is exact and matches the chosen path.
struct tag8_closure {
    uint8_t which;  // offset 0
};

__attribute__((noinline)) static void tag8_small_path(void*) {
    volatile char buf[40];
    buf[0] = 1;
}

__attribute__((noinline)) static void tag8_large_path(void*) {
    volatile char buf[512];
    for (int i = 0; i < 512; ++i) buf[i] = static_cast<char>(i);
}

__attribute__((noinline)) static void tag8_caller(void* data) {
    volatile char buf[24];
    buf[0] = 1;
    auto* c = static_cast<tag8_closure*>(data);
    if (c->which == 0) {
        tag8_small_path(nullptr);
    } else {
        tag8_large_path(nullptr);
    }
}

TEST_CASE("LDRB-byte-tag-prunes-exact") {
    tag8_closure c_small{0};
    auto r_small = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&tag8_caller), &c_small);
    CHECK(r_small.is_exact);
    CHECK(r_small.max_depth > 0);
    // Small path stays well under the large path's 512-byte buffer.
    CHECK(r_small.max_depth < 512);

    tag8_closure c_large{1};
    auto r_large = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&tag8_caller), &c_large);
    CHECK(r_large.max_depth > r_small.max_depth);
    MESSAGE("tag8_caller (small) depth: ", r_small.max_depth,
            ", is_exact: ", r_small.is_exact,
            "; (large) depth: ", r_large.max_depth,
            ", is_exact: ", r_large.is_exact);
}

// Case (R-X1): a callable-in-X1 indirection pattern. The closure stores the
// callable; the caller routes the closure pointer through a callee-saved
// register across a BL boundary that clobbers X0–X7, then reloads it. The
// MOV X_to_X tracking (pre-existing) + T3.4.2's PC_RELATIVE promotion let
// the post-BL BLR resolve to a concrete target. This exercises the
// "provenance survives X0 clobber" arm of T3.4.3's H4 fix.
__attribute__((noinline)) static void x1_callable_target(void*) {
    volatile char buf[40];
    buf[0] = 1;
}

__attribute__((noinline)) static void x1_aux(void) {
    volatile char buf[24];
    buf[0] = 1;
}

struct x1_closure { void (*cb)(void*); };

__attribute__((noinline)) static void x1_caller(void* data) {
    volatile char buf[32];
    buf[0] = 1;
    auto* c = static_cast<x1_closure*>(data);
    x1_aux();        // BL — clobbers X0–X7; closure ptr must transit X19.
    c->cb(nullptr);  // call through closure-held callable
    // Add a trailing statement so the codegen uses BLR (not BR tail-call)
    // for the indirect dispatch — exercises the same handler shape as a
    // mid-function virtual call.
    buf[1] = 2;
}

TEST_CASE("Callable-survives-X0-clobber-via-callee-saved") {
    x1_closure c{&x1_callable_target};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&x1_caller), &c);
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("x1_caller depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// ---- 🎯T3.10: per-register provenance forwarding across BL ----

// Case (R-X1 literal): a callable passed as a function-pointer *argument*
// that arrives in the callee's X1 (per AAPCS64 `f(int n, void(*cb)(void*))`,
// where the integer occupies X0 and the callable occupies X1), is moved to a
// callee-saved register in the prologue (MOV X19, X1) across a BL that
// clobbers X0–X7, then invoked via BLR X19. Distinct from the closure-*held*
// shape above: here cb crosses a real BL boundary as an argument register,
// and the BLR happens in the *callee*, so the callee's analyser must be
// seeded with X1 — the parent cannot normalise it into X0.
//
// The callable is loaded from the opaque `data` pointer in the caller (not a
// compile-time constant) so the compiler cannot devirtualise `cb()` into a
// direct call or constant-propagate it into the callee — the indirect
// dispatch genuinely survives to runtime, exactly as it does for a spawned
// closure. Pre-T3.10 the callee's analyser only ever seeded X0, so X1 (hence
// X19) stayed UNKNOWN and the BLR fell to budget. With per-register
// forwarding the parent's BL site seeds the callee's X1 with the callable's
// resolved (PC_RELATIVE) address, so MOV X19, X1 carries it and the BLR
// resolves to a direct call.
struct rx1_lit_args { void (*cb)(void*); };  // cb at offset 0

__attribute__((noinline)) static void rx1_lit_target(void*) {
    volatile char buf[40];
    buf[0] = 1;
}

__attribute__((noinline)) static void rx1_lit_aux(void) {
    volatile char buf[24];
    buf[0] = 1;
}

__attribute__((noinline))
static void rx1_lit_callee(int n, void (*cb)(void*)) {
    volatile char buf[32];
    buf[0] = static_cast<char>(n);
    rx1_lit_aux();   // BL — clobbers X0–X7; cb must transit a callee-saved reg.
    cb(nullptr);     // BLR through the callable that arrived in X1.
    buf[1] = 2;      // trailing statement → BLR, not a BR tail-call.
}

__attribute__((noinline)) static void rx1_lit_caller(void* data) {
    volatile char buf[16];
    buf[0] = 1;
    auto* a = static_cast<rx1_lit_args*>(data);
    rx1_lit_callee(7, a->cb);  // cb (opaque) passed in X1 across a real BL.
    buf[1] = 2;  // trailing statement → real BL, not a tail-call B.
}

TEST_CASE("R-X1 literal pattern") {
    rx1_lit_args a{&rx1_lit_target};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&rx1_lit_caller), &a);
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    MESSAGE("rx1_lit_caller depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

// Case (R-CONST-arg): a discriminator passed as an integer *argument* that
// arrives in the callee's W0, on which the callee branches (CBZ) before any
// inner BL. The value is loaded from the opaque `data` pointer in the caller,
// so the compiler cannot constant-fold the callee's branch — the CBZ
// genuinely survives. With CONST forwarding the parent seeds the callee's
// argument register, so the callee prunes to the taken arm and never explores
// the deep arm. The "read before inner BL" precondition is enforced for free:
// any nested BL in the callee clobbers X0–X7 to UNKNOWN, after which the
// branch is no longer pruned (paper 30 §6.2).
struct rconst_args { int which; };  // which at offset 0

__attribute__((noinline)) static void rconst_small_arm(void*) {
    volatile char buf[40];
    buf[0] = 1;
}

__attribute__((noinline)) static void rconst_large_arm(void*) {
    volatile char buf[512];
    for (int i = 0; i < 512; ++i) buf[i] = static_cast<char>(i);
}

__attribute__((noinline)) static void rconst_callee(int which) {
    volatile char buf[24];
    buf[0] = 1;
    if (which == 0) {
        rconst_small_arm(nullptr);
    } else {
        rconst_large_arm(nullptr);
    }
}

__attribute__((noinline)) static void rconst_caller(void* data) {
    volatile char buf[16];
    buf[0] = 1;
    auto* a = static_cast<rconst_args*>(data);
    rconst_callee(a->which);  // which (opaque) passed in W0 across a real BL.
    buf[1] = 2;  // trailing statement → real BL, not a tail-call B.
}

// ---- 🎯T52.1: call-depth capacity and cycle-set degradation ----

// Families of distinct noinline functions forming deep BL chains. Each test
// below uses its own Tag: analysis results are cached per function address,
// and a chain analysed under one depth limit caches results poisoned by that
// limit, so families must never be shared across tests. The volatile buffer
// (written with the family/link constants, so no two links fold to identical
// code) and the trailing statement keep every link a real BL frame — a tail
// call B would be followed inline by the walker and never enter the
// evaluator's on-stack set, which is what these tests exercise.
template <int Tag, int N>
__attribute__((noinline)) static void chain_fn(void* data) {
    volatile char buf[16];
    buf[0] = static_cast<char>(Tag + N);
    if constexpr (N > 0) {
        chain_fn<Tag, N - 1>(data);
    }
    buf[1] = 2;  // trailing statement → real BL, not a tail-call B
}

TEST_CASE("Chain-within-depth-limit-is-exact") {
    // Control: a 10-function chain resolves exactly under the default
    // 64-deep call-following limit.
    auto r = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&chain_fn<0, 9>));
    CHECK(r.is_exact);
    CHECK(r.max_depth > 0);
    MESSAGE("chain<9> depth: ", r.max_depth, ", is_exact: ", r.is_exact);
}

TEST_CASE("max_call_depth-is-wired") {
    // The same 10-function shape under max_call_depth=4 must degrade to a
    // budgeted, inexact result — the option is wired to the on-stack
    // capacity, not decorative.
    csp::stack_analysis_options opts;
    opts.max_call_depth = 4;
    auto r = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&chain_fn<100, 9>), nullptr, opts);
    CHECK_FALSE(r.is_exact);
    CHECK(r.max_depth > 0);
    MESSAGE("chain<9> (max_call_depth=4) depth: ", r.max_depth,
            ", is_exact: ", r.is_exact);
}

TEST_CASE("Beyond-64-deep-chain-degrades-to-inexact") {
    // Regression for the small_ptr_set capacity bug: with 70 distinct
    // functions simultaneously on-stack, insert() used to no-op silently
    // past 64 entries, quietly degrading cycle detection. Now the 65th
    // entry is refused exactly like a detected cycle: budget + inexact.
    auto r = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&chain_fn<200, 69>));
    CHECK_FALSE(r.is_exact);
    CHECK(r.max_depth > 0);
    MESSAGE("chain<69> depth: ", r.max_depth, ", is_exact: ", r.is_exact);
}

// A >64-function call cycle whose closing edge lies wholly beyond the
// 64-entry on-stack window: fn<0> calls back to fn<5>, so the cycle members
// (5..0) were never inserted when entered from fn<69> under the old
// silently-degrading set — contains() could not see them and the evaluator
// churned until the value-stack cap. With the fix the walk refuses at the
// capacity boundary long before the cycle closes.
template <int N>
__attribute__((noinline)) static void cyc_fn(void* data) {
    volatile char buf[16];
    buf[0] = static_cast<char>(N);
    if constexpr (N > 0) {
        cyc_fn<N - 1>(data);
    } else {
        cyc_fn<5>(data);  // closes the cycle (never executed; analysis only)
    }
    buf[1] = 2;  // trailing statement → real BL, not a tail-call B
}

TEST_CASE("Beyond-64-cycle-terminates-inexact") {
    auto r = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&cyc_fn<69>));
    CHECK_FALSE(r.is_exact);
    CHECK(r.max_depth > 0);
    MESSAGE("cyc_fn<69> depth: ", r.max_depth, ", is_exact: ", r.is_exact);
}

// The pure tail-call variant of the same >64 cycle: every link is a direct
// tail call (B), which the walker follows inline; the {pc, sp_delta} visit
// set terminates the flat-SP cycle. No exactness assertion — codegen may
// legitimately emit BL for some links (then the on-stack cap refuses
// instead) — the regression is that analysis terminates at all.
template <int N>
__attribute__((noinline)) static void pure_tail_fn(void* data) {
    if constexpr (N > 0) {
        return pure_tail_fn<N - 1>(data);
    } else {
        return pure_tail_fn<5>(data);  // closes the cycle
    }
}

TEST_CASE("Pure-tail-call-cycle-terminates") {
    auto r = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&pure_tail_fn<69>));
    CHECK(r.max_depth < (1u << 20));  // terminated with a sane bound
    MESSAGE("pure_tail_fn<69> depth: ", r.max_depth,
            ", is_exact: ", r.is_exact);
}

// ---- 🎯T52.1: MOV-to-SP-lowered alloca must not be invisible ----
//
// Clang lowers a runtime alloca as `mov x9, sp; sub xN, x9, x8;
// mov sp, xN` — never `SUB SP, SP, Xn` — so SP is written through the
// ADD-immediate MOV-to-SP alias from a register. The pre-T52.1 walker
// treated that as a no-op: the T52.6 unwind spike measured
// {max_depth=16, is_exact=true} for this entry with 64 KiB live. The
// closed-world SP-write detector must refuse the write: is_exact=false
// with a budget contribution. The size is runtime-derived through the
// opaque data pointer so the alloca cannot be const-folded into a static
// frame; the data object is pointer-width per the audit's fixture
// contract (the walker models data reads as at least 64 bits wide).
struct alloca_data { int n; int _pad; };

__attribute__((noinline)) static void alloca_use_buf(char* p, int n) {
    for (int i = 0; i < n; i++) p[i] = static_cast<char>(i);
    asm volatile("" ::"r"(p) : "memory");
}

__attribute__((noinline)) static void alloca_entry(void* d) {
    int n = static_cast<alloca_data*>(d)->n;
    char* p = static_cast<char*>(__builtin_alloca(n));
    alloca_use_buf(p, n);
}

TEST_CASE("Runtime-alloca-forces-inexact") {
    alloca_data d{65536, 0};
    auto r = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&alloca_entry), &d);
    CHECK_FALSE(r.is_exact);
    // The refused SP write contributes the indirect-call budget, not a
    // silent small constant.
    CHECK(r.max_depth >= 2048);
    MESSAGE("alloca_entry depth: ", r.max_depth, ", is_exact: ", r.is_exact);
}

TEST_CASE("CONST-arg-prunes-across-BL") {
    rconst_args a{0};
    auto result = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&rconst_caller), &a);
    // The forwarded CONST(0) prunes the callee's CBZ to the small arm: the
    // walk is exact and never accounts the 512-byte large arm.
    CHECK(result.is_exact);
    CHECK(result.max_depth > 0);
    CHECK(result.max_depth < 512);
    MESSAGE("rconst_caller depth: ", result.max_depth,
            ", is_exact: ", result.is_exact);
}

} // TEST_SUITE

#endif // !CSP_SANITIZED
#endif // __aarch64__
