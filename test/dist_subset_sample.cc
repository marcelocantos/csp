// Subset-link sample exercised by scripts/subset_check.sh (🎯T23.3).
// Each protocol referenced here keeps that protocol's drop-in TU alive
// through the linker; protocols not referenced should be dropped via
// -ffunction-sections + -Wl,-dead_strip / --gc-sections. The companion
// script links this with the appropriate subset of csp_<proto>.cpp drop-in
// files and uses `nm` to verify that unselected protocols' third-party
// library symbols are absent from the final binary.

#include "csp.h"

#include <cstdio>

// Volatile global keeps the optimiser from folding away the address-takes
// below; the linker treats the right-hand side as a live reference.
namespace { void* volatile sink = nullptr; }

int main() {
    // Channels always work — they're in the core TU and don't depend on
    // any third-party library. The compile-time gates below add protocol
    // symbol references for each subset under test.
    csp::run([] {
        auto [w, r] = csp::chan<int>{};
        csp::spawn([o = std::move(w)] mutable { o << 42; });
        (void)r.read();
    });

    // Force-take addresses of protocol entry points. Assigning each
    // function pointer to the volatile `sink` above keeps the optimiser
    // from folding the address away; the linker then treats each
    // assignment's right-hand side as a live reference, which pulls in
    // the entire transitive symbol set (csp_<proto>.cpp → its third-
    // party library).

#ifdef SUBSET_HTTP
    sink = reinterpret_cast<void*>(
        static_cast<csp::http::server (*)(uint16_t, csp::http::serve_options)>(
            &csp::http::serve));
#endif

#ifdef SUBSET_WS
    sink = reinterpret_cast<void*>(&csp::ws::upgrade);
#endif

#ifdef SUBSET_QUIC
    sink = reinterpret_cast<void*>(
        static_cast<csp::quic::listener (*)(uint16_t, csp::quic::listen_options)>(
            &csp::quic::listen));
#endif

#ifdef SUBSET_HTTP2
    sink = reinterpret_cast<void*>(
        static_cast<csp::http2::server (*)(uint16_t, csp::http2::serve_options)>(
            &csp::http2::serve));
#endif

#ifdef SUBSET_HTTP3
    sink = reinterpret_cast<void*>(
        static_cast<csp::http3::server (*)(uint16_t, csp::http3::serve_options)>(
            &csp::http3::serve));
#endif

    std::puts("subset sample: OK");
    return 0;
}
