// Compare switch strategies under register pressure.
// VARIANT: 0=boost fcontext, 1=light (full clobber), 2=mid (x19-x24 saved)
// LIVE: number of uint64 accumulators held live across every switch.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

extern "C" {
    struct xfer { void* fctx; void* data; };
#if VARIANT == 0
    xfer jump_fcontext(void* to, void* vp);
    void* make_fcontext(void* sp, size_t size, void (*fn)(xfer));
    static inline xfer sw(void* to, void* d) { return jump_fcontext(to, d); }
    static inline void* mk(char* top, size_t sz, void (*fn)(xfer)) { return make_fcontext(top, sz, fn); }
#elif VARIANT == 1
    xfer light_jump(void* to, void* data) asm("_light_jump");
    void* light_make(void* stack_top, void (*entry)(xfer)) asm("_light_make");
    static inline xfer sw(void* to, void* d) {
        register void* r0 asm("x0") = to;
        register void* r1 asm("x1") = d;
        asm volatile("bl _light_jump" : "+r"(r0), "+r"(r1) : :
            "x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12",
            "x13","x14","x15","x16","x17","x19","x20","x21","x22","x23",
            "x24","x25","x26","x27","x28","lr",
            "v0","v1","v2","v3","v4","v5","v6","v7","v8","v9","v10","v11",
            "v12","v13","v14","v15","v16","v17","v18","v19","v20","v21",
            "v22","v23","v24","v25","v26","v27","v28","v29","v30","v31",
            "cc","memory");
        return {r0, r1};
    }
    static inline void* mk(char* top, size_t, void (*fn)(xfer)) { return light_make(top, fn); }
#else
    xfer mid_jump(void* to, void* data) asm("_mid_jump");
    void* mid_make(void* stack_top, void (*entry)(xfer)) asm("_mid_make");
    static inline xfer sw(void* to, void* d) {
        register void* r0 asm("x0") = to;
        register void* r1 asm("x1") = d;
        asm volatile("bl _mid_jump" : "+r"(r0), "+r"(r1) : :
            "x2","x3","x4","x5","x6","x7","x8","x9","x10","x11","x12",
            "x13","x14","x15","x16","x17","x25","x26","x27","x28","lr",
            "v0","v1","v2","v3","v4","v5","v6","v7","v8","v9","v10","v11",
            "v12","v13","v14","v15","v16","v17","v18","v19","v20","v21",
            "v22","v23","v24","v25","v26","v27","v28","v29","v30","v31",
            "cc","memory");
        return {r0, r1};
    }
    static inline void* mk(char* top, size_t, void (*fn)(xfer)) { return mid_make(top, fn); }
#endif
}

static constexpr long N = 20'000'000;

static void fiber(xfer t) {
    for (;;) t = sw(t.fctx, t.data);
}

int main() {
    constexpr size_t SZ = 256 * 1024;
    void* stk = malloc(SZ);
    void* ctx = mk(static_cast<char*>(stk) + SZ, SZ, fiber);
    uint64_t acc[LIVE > 0 ? LIVE : 1] = {};
    auto t0 = std::chrono::steady_clock::now();
    xfer t{ctx, nullptr};
    for (long i = 0; i < N; i++) {
        t = sw(t.fctx, nullptr);
        // LIVE accumulators consumed and updated each iteration →
        // live across the switch, forced toward callee-saved regs.
        for (int j = 0; j < LIVE; j++) acc[j] += uint64_t(j) ^ uintptr_t(t.fctx);
    }
    auto t1 = std::chrono::steady_clock::now();
    uint64_t sum = 0;
    for (int j = 0; j < (LIVE > 0 ? LIVE : 1); j++) sum += acc[j];
    printf("variant=%d live=%2d: %6.2f ns/rt  (sum=%llu)\n", VARIANT, LIVE,
           std::chrono::duration<double, std::nano>(t1 - t0).count() / N,
           (unsigned long long)sum);
    return 0;
}
