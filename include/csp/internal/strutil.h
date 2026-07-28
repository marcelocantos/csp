// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cctype>
#include <cstddef>
#include <string>

namespace csp::internal {

// Case-insensitive ASCII string equality (🎯T48: single-sourced; formerly
// copy-pasted into http.cc, http2.cc, and ws.cc).
[[nodiscard]] inline bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Lowercase an ASCII string.
[[nodiscard]] inline std::string to_lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace csp::internal
