#pragma once

#include <csp/csp.h>
#include <csp/fcontext.h>
#include <csp/internal/stack_pool.h>

#include <any>
#include <atomic>
#include <cstddef>
#include <unordered_map>

#ifndef _WIN32
#include <unistd.h>  // write() for stack overflow message
#endif

// MSVC-compatible replacements for GCC/Clang builtins.
#ifdef _MSC_VER
#include <intrin.h>
#define CSP_UNREACHABLE() __assume(0)
#define CSP_FRAME_ADDRESS() _AddressOfReturnAddress()
#else
#define CSP_UNREACHABLE() __builtin_unreachable()
#define CSP_FRAME_ADDRESS() __builtin_frame_address(0)
#endif

// ASan fiber annotations: tell ASan about user-mode context switches
// so its fake-stack (detect_stack_use_after_return) bookkeeping stays
// consistent across jump_fcontext.  Without these, ASan can free a
// suspended fiber's fake-stack frames, causing SEGV when another
// thread reads stack-resident data (e.g. a waiter's ChanOp).
#if defined(__SANITIZE_ADDRESS__)
#define CSP_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CSP_ASAN 1
#endif
#endif
#ifdef CSP_ASAN
extern "C" {
    void __sanitizer_start_switch_fiber(void **fake_stack_save,
                                        const void *bottom, size_t size);
    void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                         const void **bottom_old,
                                         size_t *size_old);
}
#endif

// TSan fiber annotations: tell TSan about user-mode context switches
// so it can correctly track happens-before across imp switches.
#if defined(__SANITIZE_THREAD__)
#define CSP_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CSP_TSAN 1
#endif
#endif
#ifdef CSP_TSAN
extern "C" {
    void *__tsan_get_current_fiber(void);
    void *__tsan_create_fiber(unsigned flags);
    void __tsan_destroy_fiber(void *fiber);
    void __tsan_switch_to_fiber(void *fiber, unsigned flags);
}
#endif

using namespace csp;

namespace csp::detail {

enum class Status : intptr_t { run, sleep, detach, exit, spawn };

struct Imp;

// Non-inline accessors for the per-thread current-imp pointer.
// Defined in csp_globals.cpp.  The cross-TU function call prevents
// the compiler from caching the thread-pointer register (TPIDR_EL0
// on ARM64, FS/GS on x86) across jump_fcontext, which would cause
// stale TLS access when an imp resumes on a different OS thread.
Imp* current_imp();
void set_current_imp(Imp* p);

void do_switch(Status status = Status::sleep);

struct alignas(16) Imp {
    struct alignas(16) StackSlot { char c[16]; };

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    static constexpr size_t stack_size = 128 << 10;  // sanitizers need ~4x headroom
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    static constexpr size_t stack_size = 128 << 10;
#else
    static constexpr size_t stack_size = 32 << 10;
#endif
#else
    static constexpr size_t stack_size = 32 << 10;
#endif

    Imp * prev_;
    Imp * next_;
    std::atomic<fcontext_t> ctx_;
    StackRegion stk_;
    char status_[32];
    internal::ChanOp const * chanops_;
    int n_chanops_, signal_;

    size_t id_ = []{
        static std::atomic<size_t> counter{0};
        return counter++;
    }();

    Imp(fcontext_t ctx, StackRegion stk);
    Imp();
    ~Imp();
    Imp(Imp const &) = delete;

    Imp & operator=(Imp const &) = delete;

    char const * getfullstatus_() const {
        return status_;
    }

    void schedule(bool make_current = false);
    void schedule_local(bool make_current = false);
    void make_runnable();
    void deschedule();

    void run(Status status = Status::sleep);

    enum AltState : uint32_t { ALT_IDLE, ALT_WAITING, ALT_CLAIMED };
    std::atomic<uint32_t> alt_state{ALT_IDLE};

    uintptr_t dyn_ctx_{0};  // HAMT root for dynamic scope
    std::unordered_map<uint64_t, std::any>* local_ctx_{nullptr};  // imp_local storage

    // Software stack overflow limit (arena mode only).
    // Points to the bottom of the usable region (above the software guard zone).
    // Null for non-arena stacks (hardware guard page used instead).
    // Checked at every CSP suspend checkpoint via check_stack_overflow().
    void* stack_overflow_limit_ = nullptr;

    // 🎯T3.4.4: entry function and stack top recorded at spawn time so
    // suspend checkpoints can compute the running depth and update the
    // per-entry high-water table. entry_fn_ is null for the main thread's
    // synthetic imp (no spawn() called).
    ::csp::internal::EntryFn entry_fn_ = nullptr;
    void* entry_sp_ = nullptr;

    bool in_global_ = false;  // true while in the global run queue
    bool daemon_ = false;     // daemon imps don't prevent schedule() from returning
    class quiescence_scope* qs_ = nullptr;  // quiescence scope (inherited by children)
    bool qs_entered_ = false;               // true if enter() was called (vs propagate-only)
    std::atomic<bool> qs_sleeping_{false};   // true after leave(), cleared by enter at schedule or resume

    // Single-word suspend/wake protocol (🎯T34 O2, TLA:DrainSuspended):
    //   SUSP_IDLE    — not in a suspend window
    //   SUSP_PENDING — announced suspend; context save not yet drained
    //   SUSP_WAKE    — a waker arrived during the window; CheckWP or
    //                  drain_suspended will queue the imp
    // All transitions are CAS/exchange on this word, so the drain no
    // longer serializes on the global runtime mutex (paper 33 O2).
    enum SuspendState : uint32_t { SUSP_IDLE, SUSP_PENDING, SUSP_WAKE };
    std::atomic<uint32_t> suspend_state_{SUSP_IDLE};

    // Run-queue placement claim (🎯T34 round 2, TLA:PlacementClaim):
    // TRUE while the imp is in (or committed to) a run queue.  Racing
    // placers — duplicate wakers, the deferred-wake drain — each do
    // one exchange(TRUE); only the one that observed FALSE inserts.
    // Cleared by the imp itself when it delinks to suspend/exit
    // (after the CheckWP early-wake decision, so a woken-early imp
    // never exposes a FALSE window).  Replaces the global_mu-serialized
    // next_/in_global_ checks on the wake path.
    std::atomic<bool> placed_{false};

#if CSP_ASAN
    void* asan_fake_stack_ = nullptr;  // ASan fake-stack state for this fiber
#endif
#if CSP_TSAN
    void* tsan_fiber_ = nullptr;  // TSan fiber handle for this imp
#endif
};

inline
char const * getfullstatus(Imp const * imp) {
    return imp ? imp->getfullstatus_() : "O";
}

inline
char const * getstatus(Imp const * imp) {
    return getfullstatus(imp);
}

// Software stack overflow detection for arena-mode stacks.
// Called at every CSP API suspend checkpoint (yield, channel ops, spawn).
// Terminates the process with a descriptive message if the stack pointer
// has reached below the software guard zone of an arena slot.
//
// For non-arena stacks (stack_overflow_limit_ == nullptr), this is a no-op;
// the hardware guard page handles overflow.
// 🎯T3.4.4: per-entry-function stack high-water table. Recording happens at
// every suspend checkpoint where check_stack_overflow already samples SP.
// Lookup is consulted at spawn time to pick a tight slot when the analyser
// is inexact but we have empirical evidence of how deep this entry function
// goes. The 50% margin is applied at the spawn-time consumer, not here, so
// this layer reports raw observed bytes.
void record_stack_high_water(::csp::internal::EntryFn fn, size_t depth_bytes);
size_t get_stack_high_water(::csp::internal::EntryFn fn);

[[gnu::always_inline]] inline
void check_stack_overflow(Imp const* imp, void* current_sp) {
    if (!imp->stack_overflow_limit_) [[likely]] {
        return;
    }
    if (current_sp < imp->stack_overflow_limit_) [[unlikely]] {
        // Abort immediately -- the stack has corrupted the software guard zone.
        // Use a low-level write + trap to avoid additional stack use that
        // might corrupt adjacent imp stacks.
        static constexpr char msg[] =
            "csp: stack overflow detected (arena soft guard)\n";
#ifdef _WIN32
        __debugbreak();
#else
        (void)::write(2, msg, sizeof(msg) - 1);
        __builtin_trap();
#endif
    }
}

}
