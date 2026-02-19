#pragma once

#include <csp/part/map.h>
#include <csp/part/switch_all.h>

namespace csp::part {

// Maps each input to a sub-stream via f, switching to the latest.
// Previous sub-streams are cancelled when a new one arrives.
// Equivalent to map<A, reader<B>>(f) | switch_all<B>().
template <typename A, typename B, typename F>
auto switch_map(F&& f) {
    return map<A, reader<B>>(std::forward<F>(f)) | switch_all<B>();
}

}
