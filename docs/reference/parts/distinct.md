# csp::part::distinct

Suppresses consecutive duplicate values from a stream. Non-adjacent duplicates
pass through. An optional equality comparator can be supplied (default:
`std::equal_to<T>`).

## Signature

```cpp
template <typename T, typename Eq = std::equal_to<T>>
auto distinct(Eq eq = {});
// Returns: filter<T, T, ...>
```

## Topology

<!-- csp-flow
reader<T> -> {distinct(eq)} -> reader<T>
-->
![distinct topology](diagrams/distinct.svg)

One internal imp reads from the input, compares each value with the
previously emitted value, and forwards it only when it differs.

## Semantics

- Exits when the input is exhausted or the output reader is dropped.
- Tracks only the most recently emitted value; non-adjacent duplicates are
  *not* suppressed. For global deduplication, use `unique`.
- The first element is always emitted (there is no previous value to compare).
- Comparison uses `eq(prev, current)`. The default is `std::equal_to<T>`,
  which calls `operator==`.
- Values are moved into the output channel.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

auto d = distinct<int>().spawn();

// Writer sends: 1, 1, 2, 2, 3, 1, 1
// Reader gets:  1, 2, 3, 1
//   (non-adjacent duplicate 1 passes through)

// Custom case-insensitive string comparison:
auto ci_eq = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(a[i]) != std::tolower(b[i])) return false;
    return true;
};
auto d2 = distinct<std::string>(ci_eq).spawn();
// "Foo", "foo", "Bar", "bar", "BAR" -> "Foo", "Bar"
```

## See Also

- [unique](unique.md) -- global deduplication with optional bounded memory
- [where](where.md) -- filter by arbitrary predicate
