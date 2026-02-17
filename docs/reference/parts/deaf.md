# deaf

A consumer that never reads any values. Spawning it creates a writer endpoint
that is permanently blocked -- any attempt to write to it will block
indefinitely. The internal microthread simply waits for the channel to close
via `alt(~in)`.

## Signature

```cpp
template <typename T>
inline auto const deaf;
// Type: consumer<T, ...>
```

`deaf` is a `const` variable template, not a function. Use
`deaf<T>.spawn()` to create the writer endpoint.

## Topology

```mermaid
graph LR
    A["writer (blocked)"] -. never reads .-> B["deaf"]
```

The internal microthread does not read from the channel. It waits for the
channel to close (all writer copies dropped), then exits.

## Semantics

- The spawned writer endpoint accepts no values. Any write will block forever.
- The microthread exits only when the channel's write end is fully closed
  (all copies of the writer are dropped).
- Useful as a placeholder or default in `alt`/`prialt` expressions, where
  a write arm should be present but never selected except via its death guard.

## Example

```cpp
#include <csp/csp.h>
#include <csp/part/deaf.h>

using namespace csp;
using namespace csp::part;

// Use deaf as a default write target in an alt.
auto w = deaf<int>.spawn();
auto [give_up_w, give_up_r] = chan<>{};

spawn([w = std::move(w), give_up = std::move(give_up_r)] {
    // The write to deaf never succeeds; give_up fires instead.
    CHECK_EQ(-2, prialt(w << 42, ~give_up));
});

give_up_w = {};  // close the give-up channel
schedule();
```

## When to Use

- **Placeholder endpoints**: when an API requires a `writer<T>` but no
  consumer exists yet or is conditionally absent.
- **Alt/prialt defaults**: provide a write arm that never fires, so the
  alt resolves through a different branch (typically a death guard or
  timeout).
- **Pipeline stubs**: during development, plug `deaf` into a pipeline
  position where a real consumer will eventually go.

## See Also

- [mute](mute.md) -- reader endpoint that never produces values (the write-side counterpart)
- [blackhole](blackhole.md) -- consumer that reads and discards everything (opposite behavior)
