#pragma once

#include <csp/part/exhaust_all.h>
#include <csp/part/map.h>

namespace csp::part {

// Maps each input to a sub-stream via f, ignoring new inputs while draining.
// While a sub-stream is active, new sub-streams are discarded.
// Equivalent to map<A, reader<B>>(f) | exhaust_all<B>().
template <typename A, typename B, typename F>
auto exhaust_map(F&& f) {
    return map<A, reader<B>>(std::forward<F>(f)) | exhaust_all<B>();
}

}
