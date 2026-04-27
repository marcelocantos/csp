// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// HTTP/3 scaffolding tests (🎯T3.9)
//
// These tests verify:
//   1. The build links (nghttp3 compiled and linked correctly).
//   2. The stub entry points throw the expected "not yet implemented" error.
//   3. nghttp3 library version is accessible at runtime.
//
// Full server/client round-trip tests will be added once 🎯T3.8 (QUIC
// transport) is complete. See docs/papers/20-http3-design.md for the
// anticipated test structure.

#include "testutil.h"

#ifdef CSP_TLS

#include <csp/http3.h>

extern "C" {
#include <nghttp3/nghttp3.h>
}

#include <string>

using namespace csp;

// ---------------------------------------------------------------------------
// Build-linkage tests
// ---------------------------------------------------------------------------

TEST_SUITE("http3") {

TEST_CASE("nghttp3-version-accessible") {
    // Verify that nghttp3 compiled and linked. nghttp3_version() is always
    // available and returns a non-null pointer.
    const nghttp3_info* info = nghttp3_version(0);
    REQUIRE(info != nullptr);
    REQUIRE(info->version_str != nullptr);
    // Major version must be at least 1 (we vendor ≥ 1.15).
    CHECK(info->version_num >= 0x010f00);
}

TEST_CASE("http3-serve-stub-throws") {
    // The stub serve() must throw until T3.8 QUIC transport is wired up.
    bool threw = false;
    try {
        csp::http3::serve(0);
    } catch (const csp::error& e) {
        threw = true;
        std::string msg(e.what());
        CHECK(msg.find("not yet implemented") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("http3-fetch-stub-throws") {
    // The stub fetch() must throw until T3.8 QUIC transport is wired up.
    bool threw = false;
    try {
        csp::http3::get("https://example.com/");
    } catch (const csp::error& e) {
        threw = true;
        std::string msg(e.what());
        CHECK(msg.find("not yet implemented") != std::string::npos);
    }
    CHECK(threw);
}

TEST_CASE("http3-post-stub-throws") {
    bool threw = false;
    try {
        csp::http3::post("https://example.com/", {});
    } catch (const csp::error& e) {
        threw = true;
    }
    CHECK(threw);
}

} // TEST_SUITE("http3")

#endif // CSP_TLS
