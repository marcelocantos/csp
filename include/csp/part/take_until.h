#pragma once

#include <csp/part/take_while.h>  // detail::take_prefix

#include <utility>

namespace csp::part {

// Forward elements until pred is true (inclusive — emits the terminating element).
template <typename T, typename Pred>
auto take_until(Pred&& pred) {
    return detail::take_prefix<T>(std::forward<Pred>(pred), true, "take_until");
}

}
