#include <chrono>
#include <cstdio>
#include <cstdlib>

extern "C" {
    struct xfer { void* fctx; void* data; };
    // Assembly primitive; clobbers declared at the call site.
    xfer light_jump(void* to, void* data) asm("_light_jump");
    void* light_make(void* stack_top, void (*entry)(xfer)) asm("_light_make");
}

// Call-site wrapper: the compiler must assume every callee-saved
// register dies across the switch, so it spills only live values.
static inline xfer do_light_jump(void* to, void* data) {
    register void* r0 asm("x0") = to;
    register void* r1 asm("x1") = data;
    asm volatile(
        "bl _light_jump"
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

static constexpr long N = 20'000'000;

static void fiber(xfer t) {
    for (;;) {
        t = do_light_jump(t.fctx, t.data);
    }
}

int main() {
    constexpr size_t SZ = 256 * 1024;
    void* stk = malloc(SZ);
    void* ctx = light_make(static_cast<char*>(stk) + SZ, fiber);
    auto t0 = std::chrono::steady_clock::now();
    xfer t{ctx, nullptr};
    for (long i = 0; i < N; i++) {
        t = do_light_jump(t.fctx, nullptr);
    }
    auto t1 = std::chrono::steady_clock::now();
    printf("light_jump round-trip (2 jumps): %.2f ns\n",
           std::chrono::duration<double, std::nano>(t1 - t0).count() / N);
    return 0;
}
