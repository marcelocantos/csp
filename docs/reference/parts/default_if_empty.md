# default_if_empty

Passes all input values through unchanged. If the input closes without
producing any value, emits a single default value before closing the output.

## Signature

```cpp
template <typename T>
auto default_if_empty(T def);
// Returns: filter<T, T, ...>
```

## Topology

```mermaid
graph LR
    A[reader&lt;T&gt;] --> B["default_if_empty(def)"] --> C[reader&lt;T&gt;]
```

One internal microthread reads from the input and forwards values. When the
input closes, if no values were seen, the default value is written to the
output.

## Semantics

- Exits when the input is exhausted and any default has been emitted, or
  when the output reader is dropped.
- If the input produces at least one value, all values pass through and the
  default is never used.
- If the input produces zero values, a single copy of `def` is emitted.
- The default value is captured by move at construction time and emitted by
  copy (it is not consumed).
- Values from the input are moved into the output channel.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Non-empty input: default is ignored.
auto r = default_if_empty<int>(99)
             .spawn(count(1, 4).spawn());
// Reads: 1, 2, 3

// Empty input: default is emitted.
auto r2 = default_if_empty<int>(99)
              .spawn(merge(std::vector<reader<int>>{}).spawn());
// Reads: 99
```

## See Also

- [where](where.md) -- filter elements (may produce empty output)
- [first](first_last.md) -- take the first N elements
