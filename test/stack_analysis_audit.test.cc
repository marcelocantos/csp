// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Stack-analysis soundness + tightness audit (🎯T3.4.5).
//
// For each entry function in a curated set:
//   1. Run analyze_stack_depth(F, nullptr) → (analyser_estimate, is_exact).
//   2. Spawn F, run to completion → high-water captured by the profile
//      table (🎯T3.4.4 wires this).
//   3. Report (analyser_estimate, high_water, ratio, is_exact).
//   4. Soundness gate (HARD): if is_exact && analyser_estimate < high_water,
//      the analyser under-estimated — fail the build.
//   5. Tightness target (soft): 95% of cases should be exact with
//      analyser_estimate < 2 × high_water + 4 KB. Reported but not enforced.
//
// "Every entry function in examples/" from the paper is approximated by a
// curated set of representative shapes — the examples themselves call
// csp::spawn internally and aren't directly linkable as entry-fn
// libraries. Expanding the set is future work and doesn't gate retirement.

#include "testutil.h"

#include <csp/internal/csp_internal.h>

#include <doctest/doctest.h>

#include <atomic>
#include <sstream>

namespace {

// Constant per-function ABI overhead the analyser doesn't model:
//   - stp x29, x30, [sp, #-16]! (AAPCS frame record): 16 bytes
//   - alignment padding when the body has no local allocas
//   - stack-protector canary load + spill (Linux with -fstack-protector)
//   - per-thread TLS access glue called from the prologue
// The walker's max_depth measures only explicit SUB SP, SP, #imm
// adjustments inside the function body. The runtime high-water,
// measured from entry_sp_ (which points at the imp's Imp object at
// the top of the slot), naturally includes all prologue overhead.
// Observed values for a true noop: ~16 bytes on macOS, ~48 bytes on
// Linux arm64. 256 bytes is comfortably above both with enough slack
// for stack-protector variants and future minor codegen drift. This
// floor is for the audit test only — the slot-selection path uses
// 2× headroom + 2 KB ABI floor + sizeof(Imp), which already absorbs
// the prologue overhead.
constexpr size_t kFrameRecordOverhead = 256;


// --- Curated entry functions ---

void noop_entry(void*) {
    // The leanest possible imp body.
}

void volatile_buffer_entry(void*) {
    volatile char buf[1024];
    for (int i = 0; i < 1024; ++i) buf[i] = static_cast<char>(i);
}

void channel_send_recv_entry(void*) {
    // A small channel rendezvous — exercises the runtime's prialt path.
    auto [w, r] = csp::chan<int>();
    csp::spawn([w = w.copy()]() mutable { w << 42; });
    int v;
    r >> v;
}

void nested_calls_entry(void*) {
    // A deterministic call chain — the analyser should resolve every BL
    // and produce an exact answer.
    auto inner = +[]() -> int {
        volatile char buf[128];
        buf[0] = 1;
        return buf[0];
    };
    auto middle = +[](decltype(inner) f) -> int {
        volatile char buf[64];
        buf[0] = 1;
        return f() + buf[0];
    };
    volatile int sum = middle(inner);
    (void)sum;
}

void alt_two_chans_entry(void*) {
    // alt across two channels — non-trivial scheduler interaction.
    auto [w0, r0] = csp::chan<int>();
    auto [w1, r1] = csp::chan<int>();
    csp::spawn([w = w0.copy()]() mutable { w << 1; });
    csp::spawn([w = w1.copy()]() mutable { w << 2; });
    int v;
    csp::alt(r0 >> v, r1 >> v);
}

void yield_loop_entry(void*) {
    // A loop that goes through the suspend path several times.
    for (int i = 0; i < 4; ++i) csp::yield();
}

// --- Tightness fixtures: shapes the analyser is *designed* to size exactly ---
//
// Each loads its callable / discriminator from an opaque `data` pointer — not
// a compile-time constant — so the compiler can neither devirtualise the
// indirect call nor constant-fold the branch away (single-TU interprocedural
// constant propagation would otherwise resolve everything at compile time,
// making the test pass for the wrong reason). They are analysed and run with
// the same live `data`, exactly as csp::spawn() analyses spawn_entry<F> with
// the live closure. Each exercises one resolution path the analyser must size
// tightly: closure-held vtable dispatch (🎯T3.4.2), interprocedural data
// forwarding (🎯T3.4.3), and per-register provenance forwarding of a callable
// arriving in a callee's X1 and a CONST discriminator (🎯T3.10).

__attribute__((noinline)) static void tf_leaf(void*) {
    volatile char buf[48];
    buf[0] = 1;
}

__attribute__((noinline)) static void tf_aux(void) {
    volatile char buf[24];
    buf[0] = 1;
}

// (a) Closure-held vtable: load a function pointer from data, BLR it.
struct vt_data { void (*fn)(void*); };
vt_data g_vt{&tf_leaf};
void vtable_entry(void* data) {
    volatile char buf[32];
    buf[0] = 1;
    auto* d = static_cast<vt_data*>(data);
    d->fn(nullptr);
}

// (b) Interprocedural data forwarding: pass a sub-pointer to a callee that
//     dispatches through a function pointer inside it.
struct inner_d { void (*cb)(void*); };
struct outer_d { int tag; inner_d* inner; };
inner_d g_inner{&tf_leaf};
outer_d g_outer{0, &g_inner};
__attribute__((noinline)) static void interp_callee(void* data) {
    volatile char buf[40];
    buf[0] = 1;
    auto* d = static_cast<inner_d*>(data);
    d->cb(nullptr);
    buf[1] = 2;  // trailing statement → real BLR, not a BR tail-call.
}
void interp_entry(void* data) {
    volatile char buf[24];
    buf[0] = 1;
    auto* d = static_cast<outer_d*>(data);
    interp_callee(d->inner);  // real BL forwards the sub-pointer as the
    buf[1] = 2;               // callee's data (current_data is rebased), so
                              // the second-level deref resolves. A tail-call B
                              // would instead pin current_data at the root and
                              // collapse the chained load.
}

// (c) 🎯T3.10 per-register forwarding: a callable that arrives in the callee's
//     X1 (AAPCS64 f(int, void(*)(void*))), moved to a callee-saved register
//     across a clobbering BL, then invoked via BLR.
struct x1_data { void (*cb)(void*); };
x1_data g_x1{&tf_leaf};
__attribute__((noinline)) static void x1_callee(int n, void (*cb)(void*)) {
    volatile char buf[32];
    buf[0] = static_cast<char>(n);
    tf_aux();      // clobbers X0–X7; cb must transit a callee-saved register.
    cb(nullptr);   // BLR through the callable that arrived in X1.
    buf[1] = 2;
}
void x1_entry(void* data) {
    volatile char buf[16];
    buf[0] = 1;
    auto* d = static_cast<x1_data*>(data);
    x1_callee(7, d->cb);
    buf[1] = 2;
}

// (d) 🎯T3.10 CONST forwarding: a discriminator that arrives in the callee's
//     W0 and prunes a branch before any inner BL.
struct const_data { int which; };
const_data g_const{0};
__attribute__((noinline)) static void const_small(void*) {
    volatile char buf[40];
    buf[0] = 1;
}
__attribute__((noinline)) static void const_large(void*) {
    volatile char buf[512];
    for (int i = 0; i < 512; ++i) buf[i] = static_cast<char>(i);
}
__attribute__((noinline)) static void const_callee(int which) {
    volatile char buf[24];
    buf[0] = 1;
    if (which == 0) {
        const_small(nullptr);
    } else {
        const_large(nullptr);
    }
}
void const_entry(void* data) {
    volatile char buf[16];
    buf[0] = 1;
    auto* d = static_cast<const_data*>(data);
    const_callee(d->which);
    buf[1] = 2;
}

struct AuditCase {
    const char* name;
    csp::internal::EntryFn entry;
    const void* data;          // analysed and run with this (may be null)
    bool tightness_candidate;  // the analyser is designed to size this tightly
};

const AuditCase kCases[] = {
    // Tightness candidates — statically-resolvable bodies the analyser must
    // size exactly (is_exact) and tightly.
    {"noop",               &noop_entry,            nullptr,   true},
    {"nested_calls",       &nested_calls_entry,    nullptr,   true},
    {"vtable_closure",     &vtable_entry,          &g_vt,     true},
    {"interproc_forward",  &interp_entry,          &g_outer,  true},
    {"reg_x1_forward",     &x1_entry,              &g_x1,     true},
    {"const_arg_prune",    &const_entry,           &g_const,  true},
    // Runtime/PLT-bound shapes — the analyser correctly *widens* to budget
    // here (scheduler context switch via jump_fcontext, channel rendezvous in
    // prialt, the stack-protector __stack_chk_fail PLT call). spawn() sizes
    // these from the empirical high-water profile (which it consults before
    // the analyser), so they are soundness — not tightness — representatives:
    // no per-register forwarding can resolve a context switch or a PLT stub.
    {"volatile_buffer_1k", &volatile_buffer_entry,    nullptr, false},
    {"channel_send_recv",  &channel_send_recv_entry,  nullptr, false},
    {"alt_two_chans",      &alt_two_chans_entry,      nullptr, false},
    {"yield_loop",         &yield_loop_entry,         nullptr, false},
};

struct AuditResult {
    const char* name;
    size_t analyser_estimate;
    size_t high_water;
    bool is_exact;
    bool sound;     // !is_exact || analyser_estimate >= high_water
    bool tight;     // is_exact && analyser_estimate < 2 * high_water + 4096
    bool tightness_candidate;
};

AuditResult run_case(const AuditCase& c) {
    AuditResult r{};
    r.name = c.name;
    r.tightness_candidate = c.tightness_candidate;

    // Analyse — and below, run — with the same live data the case carries, so
    // the analyser sees what spawn() sees in production (🎯T3.10).
    auto sa = csp::analyze_stack_depth(reinterpret_cast<const void*>(c.entry),
                                       c.data);
    r.analyser_estimate = sa.max_depth;
    r.is_exact = sa.is_exact;

    csp::internal::spawn(c.entry, const_cast<void*>(c.data));
    csp::schedule();

    r.high_water = csp::detail::get_stack_high_water(c.entry);

    // Soundness: the analyser may legitimately under-report by up to
    // kFrameRecordOverhead because the runtime high-water includes the
    // AAPCS frame-record (stp x29, x30) that the walker doesn't see.
    r.sound = !r.is_exact ||
              r.analyser_estimate + kFrameRecordOverhead >= r.high_water;
    r.tight = r.is_exact && r.analyser_estimate < 2 * r.high_water + 4096;
    return r;
}

} // namespace

TEST_SUITE("StackAnalysisAudit") {

TEST_CASE("soundness-and-tightness-report") {
    size_t candidate_count = 0;
    size_t candidate_tight = 0;

    MESSAGE("Stack analysis audit — name | analyser | high-water | ratio | is_exact | sound | tight | kind");
    for (const auto& c : kCases) {
        auto r = run_case(c);

        // Soundness gate — HARD, over *every* case (tightness candidates and
        // runtime-bound shapes alike). Any exact analyser estimate that falls
        // below the observed high-water (allowing the AAPCS frame-record
        // floor) means the walker missed a path that would overflow a
        // tightly-sized stack.
        std::ostringstream violation;
        violation << "soundness violation for " << r.name
                  << ": analyser=" << r.analyser_estimate
                  << " + frame=" << kFrameRecordOverhead
                  << " < high_water=" << r.high_water
                  << " with is_exact=true";
        CHECK_MESSAGE(r.sound, violation.str());

        double ratio = r.high_water > 0
            ? static_cast<double>(r.analyser_estimate) /
              static_cast<double>(r.high_water)
            : 0.0;

        std::ostringstream row;
        row << r.name << " | "
            << r.analyser_estimate << " | "
            << r.high_water << " | "
            << ratio << " | "
            << (r.is_exact ? "true" : "false") << " | "
            << (r.sound ? "OK" : "VIOLATION") << " | "
            << (r.tight ? "tight" : "loose") << " | "
            << (r.tightness_candidate ? "candidate" : "soundness-only");
        MESSAGE(row.str());

        if (r.tightness_candidate) {
            ++candidate_count;
            if (r.tight) ++candidate_tight;
        }
    }

    // Tightness gate — measured over the statically-resolvable candidates,
    // the shapes the analyser is designed to size exactly. Runtime/PLT-bound
    // shapes are excluded: the analyser correctly widens them to budget and
    // spawn() sizes them from the empirical profile instead (paper 30 §7.3).
    // 🎯T3.10 acceptance: at least 4 of 6 curated candidates tight.
    std::ostringstream summary;
    summary << "tight candidates: " << candidate_tight << "/" << candidate_count;
    MESSAGE(summary.str());

    CHECK(candidate_count == 6);

    // The instruction walker is ARM64-only; on other architectures
    // analyze_stack_depth is a conservative stub that always returns
    // is_exact=false, so tightness is not measurable there (every candidate
    // reads "loose"). Soundness — gated above for every case — still holds,
    // because an inexact estimate is vacuously sound. Enforce the tightness
    // target only where the real walker runs.
#if defined(__aarch64__)
    CHECK(candidate_tight >= 4);
#else
    MESSAGE("tightness gate skipped — the stack walker is ARM64-only");
#endif
}

} // TEST_SUITE("StackAnalysisAudit")
