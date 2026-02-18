# reduce

Folds a channel down to a single value. Blocks the calling microthread until the
input is exhausted, then returns the final accumulator. Unlike `scan`, `reduce`
does not emit intermediate results -- it is a terminal operation.

## Signature

```cpp
template <typename T, typename S, typename F>
S reduce(reader<T> in, S init, F f);
```

## Topology

```mermaid
graph LR
    A[reader&lt;T&gt;] --> B["reduce(init, f)"] --> C["S (return value)"]
```

No microthread is spawned. `reduce` runs synchronously in the caller's
context, consuming the entire input stream before returning.

## Semantics

- Blocks until the input reader is exhausted (writer closed or dropped).
- Returns `init` unchanged if the input is empty.
- Applies `init = f(std::move(init), value)` for each input element.
- `T` and `S` can be different types.
- Because `reduce` is a blocking call that consumes the reader, it is not a
  `filter` -- it is a free function that takes a `reader<T>` directly.
- Not composable via `|` since it returns a value, not a part.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Sum 1..5 = 15
int total = reduce<int, int>(count(1, 6).spawn(), 0,
                [](int acc, int v) { return acc + v; });

// Concatenate strings.
auto r = map<int, std::string>([](int n) { return std::to_string(n); })
             .spawn(count(1, 4).spawn());
std::string s = reduce<std::string, std::string>(std::move(r), "",
                    [](std::string acc, const std::string& v) { return acc + v; });
// s == "123"
```

## See Also

- [scan](scan.md) -- running fold that emits every intermediate accumulator value
- [sink](../parts/sink.md) -- consume elements with a side-effecting function (does not produce a result)
