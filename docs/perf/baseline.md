# Runtime performance baseline

## Two kinds of number, one gate

`bench/` holds nanobench wall-clock benchmarks for the channel
operations, run with `make bench`. They are useful for answering "is this
change faster", and useless as a gate: on a machine with anything else
running, nanobench itself flags most of them unstable, with error bars
from 5 % to 23 % between runs of the same binary. Nothing can be locked
to that.

`perf/alloc_ratchet.cc` measures heap allocation counts and bytes for a
fixed workload instead. Those do not vary with machine load, and for
these workloads they do not vary at all. That is what `make perf` gates,
and `make perf` is part of `make bullseye`.

| Command | What it does |
|---|---|
| `make bench` | nanobench wall-clock benchmarks; reported, never gated |
| `make perf` | allocation ratchet against the table below |
| `./build/normal/csp_ratchet` | the same check, with the measured table on stdout |
| `./build/normal/csp_ratchet --write` | re-record the table below |

## Ratchet (locked both directions)

The band is ±1 % on both allocation count and bytes, in **either**
direction. A regression fails. An improvement also fails, until it is
re-recorded with `--write` in the same commit that earns it, so the
number never drifts silently. A workload with no row fails rather than
running unmeasured.

Each workload runs once unmeasured, to absorb first-touch costs like
stack-pool growth, then five measured times, and the minimum is taken.
The minimum rather than the mean because `prialt` occasionally allocates
one extra waiting record depending on which writer the scheduler reaches
first; the minimum is the count with no scheduling accident, and it
reproduced exactly across eight consecutive invocations.

Workload sizes are 2,000 channel operations and 500 spawns. The numbers
are per whole workload, not per operation, so an allocation added inside
a loop shows up as a large obvious delta rather than a rounding
difference.

<!-- perf-baseline:begin -->
| workload | allocs | bytes |
|---|---:|---:|
| send/recv | 11 | 1336 |
| prialt/8ch | 60 | 7552 |
| spawn | 2000 | 228000 |
<!-- perf-baseline:end -->

## What the numbers say

**Channel traffic is allocation-free in steady state.** 2,000 unbuffered
send/receive rendezvous cost 11 allocations in total, and 2,000
eight-way prialt selections cost 60. Neither scales with the operation
count: the allocations are setup, not per-op. That is the property worth
protecting, and it is exactly the kind of property that a refactor
breaks silently — one `std::function`, one `shared_ptr`, one vector
growth moved inside the loop and these become 2,000 and 16,000.

**Spawn allocates, and that is where the cost is.** 500 spawns cost
2,000 allocations and 228,000 bytes: four allocations and 456 bytes per
process. If anything here is worth attacking later, it is this, not the
channel path.

## Reference host

Apple M4 Max, 16 cores, macOS 26, Apple clang, C++20 with libc++, `-O2
-g`. Allocation counts are host-independent in principle; they are
recorded on one host because the byte totals depend on the standard
library's internal sizes.

## Wall-clock, for information only

`make bench`, taken while sibling agents were compiling (load average
around 90), so these are not a clean-machine measurement and are recorded
only to show the shape:

| Benchmark | ns/op | Error |
|---|---|---|
| send/recv | 157 | 7.0 % |
| prialt/2ch | 439 | 8.8 % |
| alt/2ch | 524 | 5.4 % |
| prialt/8ch | 795 | 5.4 % |
| alt/8ch | 886 | 10.8 % |
| buffered/1024 | 91 | 23.2 % |

The error column is the argument for gating allocations instead.
