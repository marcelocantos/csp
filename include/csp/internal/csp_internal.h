#pragma once

#include <csp/csp.h>
#include <csp/fcontext.h>
#include <csp/internal/stack_pool.h>

#include <any>
#include <atomic>
#include <cstddef>
#include <unordered_map>

// TSan fiber annotations: tell TSan about user-mode context switches
// so it can correctly track happens-before across imp switches.
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
#define CSP_TSAN 1
extern "C" {
    void *__tsan_get_current_fiber(void);
    void *__tsan_create_fiber(unsigned flags);
    void __tsan_destroy_fiber(void *fiber);
    void __tsan_switch_to_fiber(void *fiber, unsigned flags);
}
#endif

using namespace csp;

namespace csp::detail {

// On ELF platforms (Linux, etc.) the compiler may cache the TPIDR_EL0
// thread-pointer register in a callee-saved register across function calls.
// jump_fcontext saves/restores callee-saved registers, so when an imp resumes
// on a different OS thread the cached thread pointer is stale and TLS writes
// (e.g. g_imp = self) corrupt the wrong thread's slot.  Force the
// general-dynamic TLS model so every access goes through __tls_get_addr,
// which re-reads TPIDR_EL0 each time.
// macOS Mach-O TLV uses function calls by default, so this isn't needed there.
#if !defined(__APPLE__)
#define CSP_TLS_MODEL __attribute__((tls_model("global-dynamic")))
#else
#define CSP_TLS_MODEL
#endif

enum class Status : intptr_t { run, sleep, detach, exit, spawn };

struct Imp;

extern thread_local Imp * g_imp CSP_TLS_MODEL;

void do_switch(Status status = Status::sleep);

struct alignas(16) Imp {
    struct alignas(16) StackSlot { char c[16]; };

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
    static constexpr size_t stack_size = 128 << 10;  // sanitizers need ~4x headroom
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
        static std::atomic<size_t> next_{0};
        return next_++;
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
    void deschedule();

    void run(Status status = Status::sleep);

    enum AltState : uint32_t { ALT_IDLE, ALT_WAITING, ALT_CLAIMED };
    std::atomic<uint32_t> alt_state{ALT_IDLE};

    uintptr_t dyn_ctx_{0};  // HAMT root for dynamic scope
    std::unordered_map<uint64_t, std::any>* local_ctx_{nullptr};  // imp_local storage

    bool in_global_ = false;  // true while in the global run queue
    std::atomic<bool> wake_pending_{false};  // set by schedule() during suspending_ window
    std::atomic<bool> suspending_{false};  // true from unlock_all to do_switch completion

#if CSP_TSAN
    void* tsan_fiber_ = nullptr;  // TSan fiber handle for this imp
#endif
};

inline
char const * getfullstatus(Imp const * imp) {
    return imp ? imp->getfullstatus_() : "Ø";
}

inline
char const * getstatus(Imp const * imp) {
    return getfullstatus(imp);
}

}
