# csp::part::reduce

Folds a channel down to a single value. Consumes the entire input stream, then
emits the final accumulator as a single output element. Unlike `scan`, `reduce`
does not emit intermediate results.

## Signature

```cpp
template <typename T, typename S, typename F>
auto reduce(S init, F&& f);  // returns filter<T, S>
```

## Topology

<!-- csp-flow
reader<T> -> {reduce(init, f)} -> reader<S>
-->
![reduce topology](diagrams/reduce.svg)

A imp is spawned to consume the input. The output reader produces
exactly one value (the final accumulator) when the input is exhausted.

## Semantics

- Consumes the entire input stream, then emits a single value.
- Emits `init` unchanged if the input is empty.
- Applies `acc = f(std::move(acc), value)` for each input element.
- `T` (input type) and `S` (accumulator type) can be different.
- Composable via `|` since it is a `filter<T, S>`.
- Use `.spawn().single()` for terminal extraction of the result.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Sum 1..5 = 15
int total = (count(1, 6) | reduce<int, int>(0,
                [](int acc, int v) { return acc + v; })).spawn().single();

// Concatenate strings.
std::string s = (count(1, 4)
    | map<int, std::string>([](int n) { return std::to_string(n); })
    | reduce<std::string, std::string>("",
        [](std::string acc, const std::string& v) { return acc + v; })
).spawn().single();
// s == "123"
```

## See Also

- [scan](scan.md) -- running fold that emits every intermediate accumulator value
- [sink](../parts/sink.md) -- consume elements with a side-effecting function (does not produce a result)
