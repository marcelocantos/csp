# Parts Catalog

The **parts** system (`namespace csp::part`) provides composable building
blocks for constructing channel pipelines. Each part is a small, reusable unit
with well-defined channel topology:

- **producer** -- takes no input; writes to an output channel
- **filter** -- reads from one channel, writes to another
- **consumer** -- reads from a channel; produces no channel output

Parts compose via `.spawn()` chaining and the `|` pipe operator:

```cpp
auto r = (count(1, 10)
    | map<int>([](int n) { return n * 2; })
    | where<int>([](int n) { return n > 10; })
).spawn();
```

See [Combinator Framework](combinators.md) for the `producer`, `filter`,
`consumer` wrapper types, factory functions, and pipe operator semantics.

Each individual part page documents: signature, topology (Mermaid diagram),
semantics (backpressure, exit conditions, edge cases), and a minimal example.

---

## Sources

| Part | Type | Description |
|---|---|---|
| [count](parts/count.md) | producer | Arithmetic sequence (`start` to `stop` by `step`); also `count_forever` |
| [enumerate](parts/enumerate.md) | producer | Stream elements of a container or initializer list; also `cycle` |

## Randomness

| Part | Type | Description |
|---|---|---|
| [uniform_int](parts/random.md#uniform_int) | producer | Uniform random integers in \[lo, hi\] |
| [uniform_real](parts/random.md#uniform_real) | producer | Uniform random reals in \[lo, hi) |
| [bernoulli](parts/random.md#bernoulli) | producer | Random bools with configurable probability |
| [normal](parts/random.md#normal) | producer | Normally distributed values |
| [choice](parts/random.md#choice) | producer | Random picks from a container |
| [shuffle](parts/random.md#shuffle) | filter | Reservoir shuffle through a bounded buffer |

## Basic Transforms

| Part | Type | Description |
|---|---|---|
| [map](parts/map.md) | filter | Apply a function to each element (`A -> B`) |
| [try_map](parts/try_map.md) | filter | Map with exception catching; errors to a side channel |
| [where](parts/where.md) | filter | Forward only elements matching a predicate |
| [scan](parts/scan.md) | filter | Running fold (accumulator) with intermediate results |
| [diff](parts/diff.md) | filter | Successive differences: emit `curr - prev` for each adjacent pair |
| [flat_map](parts/flat_map.md) | filter | Map each element to a sub-stream, then merge |
| [flatten](parts/flatten.md) | filter | Flatten a stream of containers into individual elements |
| [reduce](parts/reduce.md) | filter | Fold entire stream to a single output value |
| [foreach_emit](parts/foreach_emit.md) | filter | Generalized scan with separate state update and extraction |
| [any_of / all_of](parts/quantify.md) | filter | Short-circuiting existential/universal quantifiers (emit single bool) |

## Windowing

| Part | Type | Description |
|---|---|---|
| [batch](parts/batch.md) | filter | Collect elements into fixed-size vectors |
| [window](parts/window.md) | filter | Sliding window emitting full contents as a vector |
| [slide](parts/slide.md) | filter | Two-channel sliding window (entering/leaving elements) |
| [nwise](parts/nwise.md) | filter | Sliding N-element window emitting tuples |
| [pairwise](parts/pairwise.md) | filter | Consecutive pairs from a stream |
| [quantize](parts/quantize.md) | filter | Batch additive values into variable-size quanta |
| [chunk_by](parts/chunk_by.md) | filter | Group consecutive elements where predicate holds between adjacent pairs |
| [frame](parts/frame.md) | filter | Fixed-size frames with timeout flush for partial frames |

## Filtering

| Part | Type | Description |
|---|---|---|
| [distinct](parts/distinct.md) | filter | Suppress consecutive duplicate values |
| [unique](parts/unique.md) | filter | Suppress all-time duplicates (hash set) |
| [take_while](parts/take_while.md) | filter | Forward elements while predicate holds, then close |
| [take_until](parts/take_until.md) | filter | Forward elements until predicate holds (inclusive — emits the match) |
| [skip_while](parts/skip_while.md) | filter | Drop leading elements while predicate holds |
| [first / last / skip_first / skip_last](parts/first_last.md) | filter | Position-based selection at stream boundaries |
| [stride](parts/stride.md) | filter | Take every Nth element |
| [default_if_empty](parts/default_if_empty.md) | filter | Emit a default value if input closes without producing any |

## Timing

| Part | Type | Description |
|---|---|---|
| [delay](parts/delay.md) | filter | Delay each value by a fixed duration |
| [debounce](parts/debounce.md) | filter | Emit only after a quiet period (suppress rapid values) |
| [throttle](parts/throttle.md) | filter | Rate-limit: forward up to N values per interval, drop excess |
| [sample](parts/sample.md) | filter | On each trigger, emit the most recent value |
| [timeout](parts/timeout.md) | filter | Close output if no value arrives within a deadline |
| [gate](parts/gate.md) | filter | Pause/resume a stream via a boolean control channel |
| [pace](parts/pace.md) | filter | Rate-limited passthrough: one value per trigger, backpressure on excess |
| [timer](parts/timer.md) | filter | Convert sleep-duration requests into fire-time outputs |

## Fan-out / Fan-in

| Part | Type | Description |
|---|---|---|
| [tee](parts/tee.md) | filter | Duplicate a stream to a side channel |
| [fanout](parts/fanout.md) | producer | Broadcast input to a dynamic set of subscribers |
| [chain](parts/chain.md) | producer | Concatenate multiple readers sequentially |
| [concat_all](parts/concat_all.md) | filter | Flatten sub-streams sequentially (each completes before the next) |
| [switch_all](parts/switch_all.md) | filter | Flatten sub-streams with latest-wins cancellation |
| [exhaust_all](parts/exhaust_all.md) | filter | Flatten sub-streams, ignoring new inputs while active |
| [merge](parts/merge.md) | producer | Non-deterministic merge of N inputs |
| [race](parts/race.md) | producer | Priority-biased merge: earlier sources win on simultaneous ready |
| [mux](parts/mux.md) | producer | Non-deterministic merge of N heterogeneous inputs into `variant` |
| [demux](parts/demux.md) | function | Split a `variant` stream into N typed readers |
| [combine_latest](parts/combine_latest.md) | producer | Emit tuple of latest values whenever any input updates |
| [zip](parts/zip.md) | producer | Combine N inputs element-wise into tuples |
| [transpose](parts/transpose.md) | producer | Dynamic-width zip: N homogeneous readers in lockstep as vectors |
| [sort_merge](parts/sort_merge.md) | producer | Merge N pre-sorted streams into one sorted output |
| [unzip](parts/unzip.md) | filter | Split a tuple stream into N independent readers |

## Routing

| Part | Type | Description |
|---|---|---|
| [round_robin](parts/round_robin.md) | filter | Distribute input across N outputs in round-robin order |
| [interleave](parts/interleave.md) | producer | Merge N inputs in strict round-robin order |
| [partition](parts/partition.md) | filter | Route elements to one of N outputs by classifier |
| [group_by](parts/group_by.md) | filter | Partition by key; each unique key spawns a sub-stream |

## Concurrency

| Part | Type | Description |
|---|---|---|
| [parallel_map](parts/parallel_map.md) | filter | Concurrent transform: fan out to N workers with demand-driven dispatch |

## Advanced

| Part | Type | Description |
|---|---|---|
| [share](parts/share.md) | producer | Broadcast a source to multiple independent subscribers |
| [first_wins](parts/first_wins.md) | producer | Read from whichever source responds first, discard the rest |
| [fallback](parts/fallback.md) | producer | Sequential failover: try each reader, use first that produces |
| [join](parts/join.md) | consumer | Block until all input channels close (barrier) |
| [latch](parts/latch.md) | filter | Hold and serve the most recent value |
| [conflate](parts/conflate.md) | filter | Merge pending values when downstream is slow |
| [killswitch](parts/killswitch.md) | filter | Forward values until a keepalive channel dies |
| [metrics](parts/metrics.md) | filter | Transparent passthrough reporting throughput stats on a side channel |
| [reorder](parts/reorder.md) | filter | Resequence an out-of-order stream by key (unbounded lookahead, contiguous keys) |

## Lifecycle

| Part | Type | Description |
|---|---|---|
| [sink](parts/sink.md) | consumer | Consume all values by applying a side-effect function |
| [blackhole](parts/blackhole.md) | consumer | Consume and discard all values |
| [deaf](parts/deaf.md) | consumer | Never reads; provides a permanently blocked writer |
| [mute](parts/mute.md) | producer | Never writes; provides a permanently blocked reader |

## I/O

| Part | Type | Description |
|---|---|---|
| [byte_reader](parts/byte_reader.md) | producer | Produce byte chunks from a non-blocking file descriptor |
| [byte_writer](parts/byte_writer.md) | consumer | Write byte chunks to a file descriptor |
| [split_lines](parts/split_lines.md) | filter | Split a byte stream into newline-delimited strings |
| [fixed_frames](parts/fixed_frames.md) | filter | Split a byte stream into fixed-size frames |

## RPC

| Part | Type | Description |
|---|---|---|
| [rpc](parts/rpc.md) | special | Request-response over channels (channel-pair and reply-in-request variants) |
