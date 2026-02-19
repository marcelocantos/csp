#pragma once

#include <csp/part/concat_all.h>
#include <csp/part/map.h>

namespace csp::part {

// Maps each input to a sub-stream via f, then drains each sequentially.
// Equivalent to map<A, reader<B>>(f) | concat_all<B>().
template <typename A, typename B, typename F>
auto concat_map(F&& f) {
    return map<A, reader<B>>(std::forward<F>(f)) | concat_all<B>();
}

}
