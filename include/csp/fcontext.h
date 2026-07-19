#pragma once

#include <cstddef>

// Types and functions matching vendored Boost.Context assembly
// (vendor/github.com/boostorg/context/src/asm/).  The assembly uses C linkage,
// so we declare extern "C" to suppress name mangling.

// 🎯T35.1: minimal-save ("light") context switch. Opt-in via
// -DCSP_LIGHT_SWITCH; active only on arm64 non-Windows builds without
// sanitizers (Boost fcontext remains the default everywhere else —
// Windows needs its TIB bookkeeping, and the sanitizer runtimes'
// fiber support is validated against the fcontext contract).
#if defined(CSP_LIGHT_SWITCH) && defined(__aarch64__) && !defined(_WIN32)
#  if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#    define CSP_USE_LIGHT_SWITCH 0
#  elif defined(__has_feature)
#    if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#      define CSP_USE_LIGHT_SWITCH 0
#    else
#      define CSP_USE_LIGHT_SWITCH 1
#    endif
#  else
#    define CSP_USE_LIGHT_SWITCH 1
#  endif
#else
#  define CSP_USE_LIGHT_SWITCH 0
#endif

namespace csp {

using fcontext_t = void *;

struct transfer_t {
    fcontext_t fctx;
    void * data;
};

extern "C" {
transfer_t jump_fcontext(fcontext_t to, void * vp);
fcontext_t make_fcontext(void * sp, std::size_t size, void (* fn)(transfer_t));
}

#if CSP_USE_LIGHT_SWITCH

#if defined(__APPLE__)
#define CSP_LIGHT_JUMP_SYM "_csp_light_jump"
#else
#define CSP_LIGHT_JUMP_SYM "csp_light_jump"
#endif

extern "C" {
fcontext_t csp_light_make(void * stack_top, void (* fn)(transfer_t));
}

// The switch assembly (src/light_switch_arm64_*.S) saves only fp/lr
// (+PC). This wrapper declares every other callee-saved register —
// x19-x28 and the full vector file — as clobbered, so the compiler
// saves exactly the values that are live at each call site instead of
// the asm unconditionally saving all 20 AAPCS callee-saved registers.
// An in-between contract (asm saves a "commonly live" subset) was
// measured and is dominated at every register-pressure level — see
// bench/lightswitch/ and paper 33 round 3.
inline transfer_t csp_jump(fcontext_t to, void * vp) {
    register void * r0 asm("x0") = to;
    register void * r1 asm("x1") = vp;
    asm volatile(
        "bl " CSP_LIGHT_JUMP_SYM
        : "+r"(r0), "+r"(r1)
        :
        : "x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12",
          "x13","x14","x15","x16","x17","x19","x20","x21","x22","x23",
          "x24","x25","x26","x27","x28","lr",
          "v0","v1","v2","v3","v4","v5","v6","v7","v8","v9","v10","v11",
          "v12","v13","v14","v15","v16","v17","v18","v19","v20","v21",
          "v22","v23","v24","v25","v26","v27","v28","v29","v30","v31",
          "cc","memory");
    return {r0, r1};
}

inline fcontext_t csp_make(void * sp, std::size_t, void (* fn)(transfer_t)) {
    return csp_light_make(sp, fn);
}

#else  // !CSP_USE_LIGHT_SWITCH

inline transfer_t csp_jump(fcontext_t to, void * vp) {
    return jump_fcontext(to, vp);
}

inline fcontext_t csp_make(void * sp, std::size_t size, void (* fn)(transfer_t)) {
    return make_fcontext(sp, size, fn);
}

#endif

}
