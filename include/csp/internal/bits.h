#pragma once

#include <bit>
#include <cassert>
#include <cstddef>

namespace csp::detail {

// Smallest power of two >= n.  n must be nonzero.
constexpr size_t round_up_pow2(size_t n) {
    assert(n > 0);
    return std::bit_ceil(n);
}

}
