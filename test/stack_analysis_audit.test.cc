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
#include <cstring>
#include <sstream>

// 🎯T31: detect whether this TU is built under a sanitizer that instruments
// memory accesses (ASan) or every load/store (TSan). Such builds inject
// interceptor calls — `bl __asan_report_loadN`, `bl __tsan_readN` — into every
// data-touching function. Those stubs live in __TEXT,__stubs, outside the
// walker's __TEXT,__text bound, so the walker soundly widens each instrumented
// candidate to budget (is_exact=false). Tightness is therefore not measurable
// under instrumentation, though soundness (an over-approximation) still holds.
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    define CSP_AUDIT_SANITIZED 1
#  endif
#endif
#if !defined(CSP_AUDIT_SANITIZED) && \
    (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#  define CSP_AUDIT_SANITIZED 1
#endif

// 🎯T52.2: painted true-peak watermark. Available exactly where the runtime
// paints stacks (CSP_STACK_PAINT, from csp_internal.h): ANALYSE arena builds.
// Where available, the hard under-estimate gate below compares the analyser
// against the painted TRUE peak, not just the checkpoint-sampled high-water
// (which structurally misses depth reached between suspend points).
#if CSP_STACK_PAINT
#  define CSP_AUDIT_PAINT 1
#endif

namespace {

// Portable "don't inline this" so each fixture keeps its own frame and the
// walker sees real BL/BLR calls. MSVC spells it differently from Clang/GCC;
// the audit file (unlike stack_analysis.test.cc) compiles on every platform
// because run_case spawns every fixture for the soundness gate, so the
// attribute must be portable even though tightness is only gated on ARM64.
#if defined(_MSC_VER)
#define AUDIT_NOINLINE __declspec(noinline)
#else
#define AUDIT_NOINLINE __attribute__((noinline))
#endif

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

// 🎯T52.2 watermark fixture: an 8 KB frame that lives and dies BETWEEN
// suspend checkpoints. The do_switch sampler only sees the shallow stack at
// the yield (and at exit), so the checkpoint high-water misses the peak
// entirely — only the painted watermark can observe it. The asm escape pins
// the buffer to a real stack slot (clang -O2 can otherwise coalesce a
// non-escaping volatile local to a few bytes; see stack_slot_sizing.test.cc).
static AUDIT_NOINLINE void deep_transient_helper() {
    volatile char buf[8192];
#if !defined(_MSC_VER)
    asm volatile("" : : "r"(const_cast<char*>(&buf[0])) : "memory");
#endif
    for (size_t i = 0; i < sizeof(buf); ++i) buf[i] = static_cast<char>(i);
}

void transient_peak_entry(void*) {
    deep_transient_helper();
    csp::yield();
}

// --- Tightness fixtures: shapes the analyser is *designed* to size exactly ---
//
// Each loads its callable / discriminator from an opaque `data` pointer — not
// a compile-time constant — so the compiler can neither devirtualise the
// indirect call nor constant-fold the branch away (single-TU interprocedural
// constant propagation would otherwise resolve everything at compile time,
// making the test pass for the wrong reason). They are analysed and run with
// the same live `data`. Production `csp::spawn<F>` analyses the concrete
// `spawn_invoke<F>` root (🎯T52.3) rather than the type-erased trampoline;
// these fixtures mirror that shape by analysing the entry with its live
// closure. Each exercises one resolution path the analyser must size
// tightly: closure-held vtable dispatch (🎯T3.4.2), interprocedural data
// forwarding (🎯T3.4.3), and per-register provenance forwarding of a callable
// arriving in a callee's X1 and a CONST discriminator (🎯T3.10).

static AUDIT_NOINLINE void tf_leaf(void*) {
    volatile char buf[48];
    buf[0] = 1;
}

static AUDIT_NOINLINE void tf_aux(void) {
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
static AUDIT_NOINLINE void interp_callee(void* data) {
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
static AUDIT_NOINLINE void x1_callee(int n, void (*cb)(void*)) {
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
//
// 🎯T31 fixture contract — the closure passed to analyze_stack_depth is
// modelled as a pointer-width-or-larger object. The walker always dereferences
// a forwarded/entry data pointer with a 64-bit load (a forwarded data
// sub-pointer is a `void*`; see OP_CALL_DIRECT_WITH_DATA in
// src/stack_analysis_arm64.cc). Under ASan the walker additionally follows the
// compiler's shadow-check slow path, whose `bl __asan_report_loadN` forwards
// the closure pointer itself — so even this fixture, whose logical payload is
// a 4-byte discriminator, has its backing object read 8 bytes wide. Real
// spawn() closures are always >= sizeof(void*); a bare 4-byte object is not a
// valid closure. `which` must stay a 4-byte int at offset 0 so it materialises
// as a CONST for branch pruning (widening it to 64 bits would make the load
// look like a pointer field and defeat the pruning this case exists to test);
// the trailing pad makes the object pointer-width so the modelled 64-bit read
// stays in bounds. Without the pad, ASan trips a global-buffer-overflow one
// byte past g_const.
struct const_data { int which; int _pad; };
const_data g_const{0, 0};
static AUDIT_NOINLINE void const_small(void*) {
    volatile char buf[40];
    buf[0] = 1;
}
static AUDIT_NOINLINE void const_large(void*) {
    volatile char buf[512];
    for (int i = 0; i < 512; ++i) buf[i] = static_cast<char>(i);
}
static AUDIT_NOINLINE void const_callee(int which) {
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

// --- 🎯T52.1 fixtures: decoded writeback forms + SP-write refuse posture ---
//
// Hand-written naked asm, so the exact peak SP displacement is known by
// construction (no compiler prologue, no sanitizer instrumentation) and the
// analyser must reproduce it bit-for-bit. Guarded to Clang/GCC on ARM64:
// MSVC has no GNU asm / naked support on ARM64.
#if defined(__aarch64__) && !defined(_MSC_VER)

// Every decoded writeback family, pre- and post-indexed, balanced so the
// fixture is executable: GP pairs (X, W), FP/SIMD pairs (S, D, Q), single
// registers (X, W, D, Q). All adjustments are multiples of 16 to respect
// hardware SP alignment. Peak depth: 224 bytes, at the pre-indexed LDP.
__attribute__((naked)) void writeback_forms_entry(void*) {
    asm volatile(
        "stp x1, x2, [sp, #-16]!\n"   // 16   GP X pair, pre
        "stp w1, w2, [sp, #-16]!\n"   // 32   GP W pair, pre
        "stp q0, q1, [sp, #-64]!\n"   // 96   SIMD Q pair, pre
        "stp d0, d1, [sp, #-32]!\n"   // 128  FP D pair, pre
        "stp s0, s1, [sp, #-16]!\n"   // 144  FP S pair, pre
        "str x1, [sp, #-16]!\n"       // 160  single X, pre
        "str w1, [sp, #-16]!\n"       // 176  single W, pre
        "str d0, [sp, #-16]!\n"       // 192  single D, pre
        "str q0, [sp, #-16]!\n"       // 208  single Q, pre
        "ldp x3, x4, [sp, #-16]!\n"   // 224  GP pair pre-indexed *load*
        "stp x3, x4, [sp], #16\n"     // 208  GP pair post-indexed *store*
        "ldr q0, [sp], #16\n"         // 192  single Q, post
        "ldr d0, [sp], #16\n"         // 176  single D, post
        "ldr w1, [sp], #16\n"         // 160  single W, post
        "ldr x1, [sp], #16\n"         // 144  single X, post
        "ldp s0, s1, [sp], #16\n"     // 128  FP S pair, post
        "ldp d0, d1, [sp], #32\n"     // 96   FP D pair, post
        "ldp q0, q1, [sp], #64\n"     // 32   SIMD Q pair, post
        "ldp w1, w2, [sp], #16\n"     // 16   GP W pair, post
        "ldp x1, x2, [sp], #16\n"     // 0    GP X pair, post
        "ret\n");
}
constexpr size_t kWritebackFixturePeak = 224;

// An SP write outside the modelled forms (MOV SP, Xn — the add-immediate
// alias with Rd=SP, Rn!=SP). The closed-world detector must refuse it:
// budget + is_exact=false, never a silent SP drift inside an exact result.
__attribute__((naked)) void sp_mov_refuse_entry(void*) {
    asm volatile(
        "mov x9, sp\n"
        "sub x9, x9, #32\n"
        "mov sp, x9\n"          // unmodelled SP write → refuse
        "add sp, sp, #32\n"
        "ret\n");
}

#endif // __aarch64__ && !_MSC_VER

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
    // 🎯T52.2: deep transient frame between suspends — checkpoint-invisible,
    // watermark-visible. The dedicated painted-watermark checks below assert
    // that the painted peak sees the 8 KB the checkpoint sampler misses.
    {"transient_peak",     &transient_peak_entry,     nullptr, false},
};

struct AuditResult {
    const char* name;
    size_t analyser_estimate;
    size_t high_water;
    size_t true_peak;  // 🎯T52.2 painted watermark peak (0 if painting off)
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
    csp::await_completion();

    r.high_water = csp::detail::get_stack_high_water(c.entry);
#ifdef CSP_AUDIT_PAINT
    // 🎯T52.2: TRUE peak from the painted watermark, scanned by destroy_imp
    // when the imp above exited (await_completion has returned, so the scan
    // is complete and published).
    r.true_peak = csp::detail::get_stack_true_peak(c.entry);
#endif

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
#ifdef CSP_AUDIT_PAINT
    // 🎯T52.2: runtime-shell floor for the painted-peak gate, measured as
    // the painted peak of the leanest possible imp ("noop", first case).
    // The watermark measures the WHOLE fiber footprint — fcontext boot
    // record, start() trampoline, and the do_switch(exit) path (which
    // under ANALYSE includes the high-water table's mutex + hash + malloc
    // frames) — while the analyser sizes only the entry body. The noop
    // peak is exactly that shared shell. kShellSlack absorbs allocator
    // path variance between runs (fast vs slow malloc path).
    //
    // 🎯T52.3: the same measured shell must sit under kShellStackBytes
    // (the production C_shell constant composed with user-entry analysis).
    size_t shell = 0;
    constexpr size_t kShellSlack = 1024;
#endif

    MESSAGE("Stack analysis audit — name | analyser | high-water | true-peak | ratio | is_exact | sound | tight | kind");
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

#ifdef CSP_AUDIT_PAINT
        if (std::strcmp(r.name, "noop") == 0) {
            shell = r.true_peak;
            // 🎯T52.3 C_shell audit: the production constant must cover the
            // measured runtime shell. If this fails, bump kShellStackBytes
            // with fresh evidence (see docs/reference/stack-analysis.md).
            CHECK_MESSAGE(shell <= csp::kShellStackBytes,
                          "measured runtime shell (" << shell
                          << ") exceeds kShellStackBytes ("
                          << csp::kShellStackBytes
                          << ") — C_shell is no longer a sound floor");
        }

        // Painting must have happened: every spawned imp leaves a non-zero
        // painted peak (the boot record alone unpaints bytes).
        CHECK_MESSAGE(r.true_peak > 0,
                      "no painted peak recorded for " << r.name);

        // Sanity: the watermark is a superset of the checkpoint sampler —
        // painted peak >= checkpoint high-water (64-byte slack for the
        // pathological case where the deepest frame's own bytes equal the
        // paint pattern).
        std::ostringstream ws;
        ws << "watermark below checkpoint high-water for " << r.name
           << ": true_peak=" << r.true_peak
           << " < high_water=" << r.high_water;
        CHECK_MESSAGE(r.true_peak + 64 >= r.high_water, ws.str());

        // Painted-peak soundness gate — HARD (🎯T52.2). The analyser bound
        // plus the measured runtime shell must cover the TRUE peak, not
        // just the checkpoint-sampled one. This is the gate the checkpoint
        // comparison above cannot provide: it sees depth reached BETWEEN
        // suspend points.
        bool sound_peak = !r.is_exact ||
            r.analyser_estimate + kFrameRecordOverhead + shell + kShellSlack
                >= r.true_peak;
        std::ostringstream pv;
        pv << "painted-peak soundness violation for " << r.name
           << ": analyser=" << r.analyser_estimate
           << " + frame=" << kFrameRecordOverhead
           << " + shell=" << shell << " + slack=" << kShellSlack
           << " < true_peak=" << r.true_peak
           << " with is_exact=true";
        CHECK_MESSAGE(sound_peak, pv.str());

        // The fixture built to defeat checkpoint sampling: its 8 KB frame
        // unwinds before the first suspend, so only the watermark sees it.
        if (std::strcmp(r.name, "transient_peak") == 0) {
            CHECK_MESSAGE(r.true_peak >= 8192,
                          "watermark missed the 8 KB transient frame: "
                          "true_peak=" << r.true_peak);
            CHECK_MESSAGE(r.true_peak > r.high_water + 4096,
                          "checkpoint high-water (" << r.high_water
                          << ") unexpectedly saw the transient peak ("
                          << r.true_peak << ")");
        }
#endif

        double ratio = r.high_water > 0
            ? static_cast<double>(r.analyser_estimate) /
              static_cast<double>(r.high_water)
            : 0.0;

        std::ostringstream row;
        row << r.name << " | "
            << r.analyser_estimate << " | "
            << r.high_water << " | "
            << r.true_peak << " | "
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
    // reads "loose"). Under a sanitizer (🎯T31) the walker widens every
    // instrumented candidate for the same reason. Soundness — gated above for
    // every case — still holds in both situations, because an inexact estimate
    // is vacuously sound. Enforce the tightness target only where the real,
    // uninstrumented walker runs.
#if defined(__aarch64__) && !defined(CSP_AUDIT_SANITIZED)
    CHECK(candidate_tight >= 4);
#elif defined(__aarch64__)
    MESSAGE("tightness gate skipped — sanitizer instrumentation injects "
            "interceptor calls the walker soundly widens on");
#else
    MESSAGE("tightness gate skipped — the stack walker is ARM64-only");
#endif
}

#if defined(__aarch64__) && !defined(_MSC_VER)

TEST_CASE("writeback-forms-exact-depth") {
    // 🎯T52.1: the decoded pair/single-register writeback forms (GP + FP/
    // SIMD, pre/post-indexed) must produce the asm fixture's exact peak —
    // equality, not a bound, because the fixture is hand-written asm.
    auto r = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&writeback_forms_entry));
    CHECK(r.is_exact);
    CHECK(r.max_depth == kWritebackFixturePeak);

    // Executability cross-check: the fixture is balanced and runs to
    // completion as a real imp (would fault on any SP misalignment).
    csp::internal::spawn(&writeback_forms_entry, nullptr);
    csp::await_completion();
}

TEST_CASE("unmodelled-sp-write-refused") {
    // 🎯T52.1: MOV SP, Xn is architecturally an SP write the walker does
    // not model; the closed-world detector must force inexact rather than
    // letting the SP delta drift inside an exact result.
    auto r = csp::analyze_stack_depth(
        reinterpret_cast<const void*>(&sp_mov_refuse_entry));
    CHECK_FALSE(r.is_exact);

    csp::internal::spawn(&sp_mov_refuse_entry, nullptr);
    csp::await_completion();
}

#endif // __aarch64__ && !_MSC_VER

} // TEST_SUITE("StackAnalysisAudit")
