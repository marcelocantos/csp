# mute

A producer that never emits any values. Spawning it creates a reader endpoint
that is permanently blocked -- any attempt to read from it will block
indefinitely. The internal imp simply waits for the channel to close
via `alt(~out)`.

## Signature

```cpp
template <typename T = poke_t>
inline auto const mute;
// Type: producer<T, ...>
```

`mute` is a `const` variable template, not a function. Use
`mute<T>.spawn()` to create the reader endpoint. The type parameter defaults
to `poke_t` for signaling channels.

## Topology

<!-- csp-flow
{mute} ..> reader (blocked)
-->
![mute topology](diagrams/mute.svg)

The internal imp does not write to the channel. It waits for the
channel to close (all reader copies dropped), then exits.

## Semantics

- The spawned reader endpoint never produces values. Any read will block
  forever.
- The imp exits only when the channel's read end is fully closed
  (all copies of the reader are dropped).
- Useful as a placeholder or default in `alt`/`prialt` expressions, where
  a read arm should be present but never selected except via its death guard.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Use mute as a default read source in an alt.
auto r = mute<int>.spawn();
auto [give_up_w, give_up_r] = chan<>{};

spawn([r = r.copy(), give_up = std::move(give_up_r)] {
    int n;
    // The read from mute never succeeds; give_up fires instead.
    CHECK_GT(0, prialt(r >> n, ~give_up));
});

give_up_w = {};  // close the give-up channel
schedule();
```

## When to Use

- **Placeholder endpoints**: when an API requires a `reader<T>` but no
  producer exists yet or is conditionally absent.
- **Alt/prialt defaults**: provide a read arm that never fires, so the
  alt resolves through a different branch (typically a death guard or
  timeout).
- **Pipeline stubs**: during development, plug `mute` into a pipeline
  position where a real producer will eventually go.

## See Also

- [deaf](deaf.md) -- consumer that never reads values (the read-side counterpart)
- [blackhole](blackhole.md) -- consumer that reads and discards everything
