#ifndef INCLUDED__csp__test__testscale_h
#define INCLUDED__csp__test__testscale_h

// Scale factor for test iteration counts under sanitizers.
// Sanitizers add significant overhead (5-15x), so we reduce iteration
// counts to keep total test time reasonable while still exercising the
// same code paths.

#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer)) \
    || defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
#define CSP_TEST_SANITIZER 1
#else
#define CSP_TEST_SANITIZER 0
#endif

#if CSP_TEST_SANITIZER
constexpr int SCALE_HEAVY = 100;   // 1M → 10K, 100K → 1K
constexpr int SCALE_MEDIUM = 10;   // 10K → 1K, 1K → 100
constexpr int SCALE_LIGHT = 2;     // 500 → 250, 100 → 50
#else
constexpr int SCALE_HEAVY = 1;
constexpr int SCALE_MEDIUM = 1;
constexpr int SCALE_LIGHT = 1;
#endif

#endif // INCLUDED__csp__test__testscale_h
