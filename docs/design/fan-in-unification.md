# Fan-in combinator unification (🎯T51)

*2026-07-31. Deferred from 🎯T49 (2026-07-28 audit).*

## Decision

| Item | Verdict |
|------|---------|
| Shared `detail::fan_in` for homogeneous fan-in | **Accepted** — backs `merge`, `join`, `first_wins`, `merge_all`; `flat_map` uses the same step/remove/push primitives |
| `flat_map = map \| merge_all` | **Rejected** — see below |
| Hot-path regression (alt/8ch, prialt/8ch) | **None** — within-epoch paired ratios ≤ 0.5% |

## Shared helper

`include/csp/part/part.h` (`namespace csp::part::detail`):

- `fan_in_step` — one `alt_begin` / typed-or-custom transfer / `alt_end` over a pre-built `ChanOp` vector
- `fan_in_push_read` / `fan_in_out_dead` — slot construction
- `fan_in_remove` — swap-and-pop of a dead reader + matching op
- `fan_in` — fixed-set homogeneous loop (build-once, mutate-in-place)

Contract preserved from the T34/T35-tuned path: **build the `ChanOp` vector once; mutate in place** (swap-and-pop on death, `push_back` on growth). Never rebuild per iteration — that is the `race.h` public-surface pattern, deliberately not used here.

`mux` / `combine_latest` keep their fixed-array disable-not-remove loops (hetero transfer table already shared in 🎯T49). `fanout` stays hand-written (control-plane + multi-type layout).

## Bench evidence (within-epoch interleaved A/B)

Platform: host macOS arm64, Clang -O2, DEBUG build (same as standing `make bench`). Two binaries (`bench-before` / `bench-after`), interleaved pairs at each `CSP_MAXPROCS`. Ratios are mean(after) / mean(before); gate is ≤ 1.05.

| P | shape | before mean (ns) | after mean (ns) | ratio |
|---:|---|---:|---:|---:|
| 2 | prialt/8ch | 212.25 | 212.60 | **1.002** |
| 2 | alt/8ch | 297.30 | 298.55 | **1.004** |
| 8 | prialt/8ch | 975.00 | 946.95 | **0.971** |
| 8 | alt/8ch | 1117.65 | 1097.75 | **0.982** |
| 16 | prialt/8ch | 1058.85 | 1045.41 | **0.987** |
| 16 | alt/8ch | 1254.60 | 1259.30 | **1.004** |

Note: `channel.bench.cc` does not call the combinators; the gate is a standing hot-path canary (same shapes used for T34/T35/T49). Part-header changes recompile the bench TU via `include/csp.h` but do not alter the alt/prialt instruction path. Earlier single-shot cross-run deltas of 40–60% at P=2 were pure epoch noise (paper 33: fan shapes are bimodal cross-epoch); interleaved pairing collapses them to <0.5%.

## Why not `flat_map = map | merge_all`

`operator|` on filters spawns the left filter as a separate imp feeding the right:

```cpp
// part.h — filter | filter
rhs(std::move(lhs).spawn(std::move(in)), std::move(out));
```

So `map | merge_all` yields:

1. **Extra imp hop** — every outer element pays map→channel→merge_all rendezvous in addition to the sub-stream merge.
2. **Teardown lag** — output death kills merge_all first; map observes `~out` one hop later. Sub-streams already handed to merge_all tear down on the same path as specialised flat_map, but the outer input and any in-flight `f(a)` that has not yet been written to merge_all are delayed by one cancellation hop.
3. **Scheduling topology** — paper 33 (postmortem §4) shows flat_map's single-imp merge is load-bearing for fairness under wake-to-local: a monopolised P growing a vector alt O(N) per outer fire was fixed by a pull-based fairness budget. Splitting map out of that imp changes who holds the growing alt and who competes for the local queue — not worth a pure-dedup win.

Composition remains a valid *user* pattern (`map-|-merge_all---same-as-flat_map` in `test/flatten_strat.test.cc` already locks the multiset semantics for non-blocking `f`). The library entry point stays the specialised single-imp body, now on the shared fan-in primitives.

## Oracles

Native / TSan / ASan results recorded in the 🎯T51 commit message.
