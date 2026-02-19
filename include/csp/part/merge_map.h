#pragma once

#include <csp/part/map.h>
#include <csp/part/merge_all.h>

namespace csp::part {

// Maps each input to a sub-stream via f, then merges all concurrently.
// Equivalent to map<A, reader<B>>(f) | merge_all<B>().
template <typename A, typename B, typename F>
auto merge_map(F&& f) {
    return map<A, reader<B>>(std::forward<F>(f)) | merge_all<B>();
}

}
