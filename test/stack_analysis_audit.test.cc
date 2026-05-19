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

struct AuditCase {
    const char* name;
    csp::internal::EntryFn entry;
};

const AuditCase kCases[] = {
    {"noop",                 &noop_entry},
    {"volatile_buffer_1k",   &volatile_buffer_entry},
    {"channel_send_recv",    &channel_send_recv_entry},
    {"nested_calls",         &nested_calls_entry},
    {"alt_two_chans",        &alt_two_chans_entry},
    {"yield_loop",           &yield_loop_entry},
};

struct AuditResult {
    const char* name;
    size_t analyser_estimate;
    size_t high_water;
    bool is_exact;
    bool sound;     // !is_exact || analyser_estimate >= high_water
    bool tight;     // is_exact && analyser_estimate < 2 * high_water + 4096
};

AuditResult run_case(const AuditCase& c) {
    AuditResult r{};
    r.name = c.name;

    auto sa = csp::analyze_stack_depth(reinterpret_cast<const void*>(c.entry));
    r.analyser_estimate = sa.max_depth;
    r.is_exact = sa.is_exact;

    csp::internal::spawn(c.entry, nullptr);
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
    constexpr size_t N = sizeof(kCases) / sizeof(kCases[0]);
    size_t tight_count = 0;

    MESSAGE("Stack analysis audit — name | analyser | high-water | ratio | is_exact | sound | tight");
    for (const auto& c : kCases) {
        auto r = run_case(c);

        // Soundness gate — HARD. Any exact analyser estimate that falls
        // below the observed high-water (allowing the AAPCS frame-
        // record floor) means the walker missed a path that would
        // overflow a tightly-sized stack.
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
            << (r.tight ? "tight" : "loose");
        MESSAGE(row.str());

        if (r.tight) tight_count++;
    }

    // Tightness target — soft. Track the achievement against the 95%
    // bar from paper 23 §8 (T3.4.5). Failing this is informational; it
    // doesn't gate retirement.
    double tight_frac = static_cast<double>(tight_count) / N;
    std::ostringstream summary;
    summary << "tight cases: " << tight_count << "/" << N
            << " (" << (tight_frac * 100) << "%) — target 95%";
    MESSAGE(summary.str());
}

} // TEST_SUITE("StackAnalysisAudit")
