# Combinator TODO

## Tier C — Coordination and resilience

- [x] **Imp death interception via dynamic scope** — Generalised
      supervision primitive. A `dynamic<writer<imp_event>>` variable
      (`imp_exit`) allows any parent to intercept child death. When an imp
      is about to die, it checks the dynamic; if a writer is present, it
      sends an event and waits for a decision — its channels stay alive
      because the imp itself hasn't terminated yet. The event carries the
      exception (null for normal exit) and exposes `restart()` /
      `restart(duration)` methods that tell the imp to re-enter its
      function from the top (same channels, fresh execution). If the event
      is dropped without calling `restart()`, or if the writer is dead,
      the imp proceeds to terminate normally (fail-fast default).

  This subsumes both the existing `worker_group` and the planned
  `errgroup`:
  - **Fail-fast (errgroup)**: No `imp_exit` binding (default) — imp dies,
    exception propagates, done.
  - **Restart policies**: An imp reads from the event channel and
    implements whatever logic it wants (sliding window, backoff,
    one-for-all, etc.).
  - **Hierarchical supervision**: Dynamic scope inheritance means each
    level can override `imp_exit` with its own channel, intercept its
    children's deaths, and only propagate to its parent by dying itself
    (escalation).

  ### Usage

  ```cpp
  // Fail-fast (default — no imp_exit binding)
  spawn(task1);
  spawn(task2);
  join(h1, h2);

  // With restart policy
  auto [ew, er] = chan<imp_event>();
  auto scope = imp_exit.local(std::move(ew));
  spawn(task1);
  spawn(task2);
  for (auto& e : er) {
      if (e.error()) e.restart();
      // normal exit: do nothing, worker stays dead
  }
  // Channel closes when all workers dead and none restarted.

  // Pre-built sliding window restart
  auto ew = restart({.max_restarts = 3, .window = 5s, .backoff = 100ms});
  auto scope = imp_exit.local(std::move(ew));
  spawn(task1);
  spawn(task2);
  ```

  `restart(...)` is sugar that spawns a policy imp and returns the writer.
  `worker_group` may survive as a convenience wrapper but is no longer
  core infrastructure.

  ### Imp as its own membrane

  The dying imp holds its channels open while it waits for the restart
  decision. This eliminates the need for a separate `supervised_spawn`
  wrapper or proxy channels for restart scenarios. The imp *is* the
  membrane: it re-enters its function on restart (same channel endpoints,
  fresh stack), or closes its channels and truly dies.

  ### Channel persistence on restart

  When `e.restart()` is called, the imp re-enters its entry function with
  the same channel endpoints. External producers/consumers never see a
  death — the channels were never closed. This is the "supervised
  endpoint" concept from the channel topology design, but achieved without
  any swap/fuse machinery: the imp simply never died.

  ### Open design questions

  - **Dynamic propagation scope**: Should `imp_exit` propagate to all
    descendants by default? A worker that spawns internal helper imps may
    not want those helpers reporting to the supervision channel. Options:
    the imp clears the dynamic before calling the user's function (opt-in
    per spawn), or helpers are expected to be short-lived enough that it
    doesn't matter.
  - **Channel state on restart**: The imp's channels are intact, but any
    in-flight data or partially completed operations from the previous
    incarnation are gone (the stack is fresh). Is this always the right
    semantic? Consider whether some form of "drain before restart" is
    needed.
  - **`restart()` response mechanism**: `restart()` on the event likely
    writes to an internal response channel that the dying imp blocks on.
    Need to nail down the exact protocol (synchronous reply vs flag).

  ### What needs building

  - `imp_event` type with `error()`, `restart()`, `restart(duration)`
  - `imp_exit` dynamic variable (`dynamic<writer<imp_event>>`)
  - Death interception in imp teardown path (check dynamic, send event,
    wait for response, re-enter or die)
  - Pre-built `restart(...)` policy helper
  - Migration path from current `worker_group` (deprecate or reimplement
    as thin wrapper over `imp_exit`)

  ### Relationship to stream combinators

  A **`retry`** part (stream-level restart) is a thin layer on top:
  ```cpp
  template <typename T>
  auto retry(std::function<reader<T>()> factory, restart_policy policy);
  // Returns reader<T>. On factory's reader death, re-invoke factory
  // per policy. Output is seamless concatenation of retried streams.
  ```

  This is the stream-shaped surface of supervision, useful as a combinator.

- [x] ~~**balance** `(reader<T>, n, F(T)→U) → reader<U>`~~ — Subsumed by
      unordered `parallel_map`.

## Tier D — Niche but elegant

- [ ] **amb** `(vector<reader<T>>) → reader<T>` — Race entire streams: commit
      to whichever source emits first, cancel the rest.

- [ ] **diff** `(reader<T>, reader<T>) → reader<diff_event<T>>` — Two-stream
      sorted differencing for change detection.

- [ ] **frame** `(reader<Chunk>, F) → reader<T>` — Byte-to-message framing
      with carry-over state.

- [ ] **repeat** `(F()→reader<T>) → reader<T>` — Re-invoke factory on normal
      completion (infinite loop). Different from retry (failure).

- [ ] **reorder** `(reader<pair<K,V>>) → reader<V>` — Reassemble out-of-order
      results by sequence number.

- [ ] **compose** `(reader<pair<A,B>>, reader<pair<B,C>>) → reader<pair<A,C>>`
      — Relational join between two keyed streams on matching middle values.

## Entropy / random parts

Implemented in `include/csp/part/random.h` (`namespace csp::part::rand`).
All producers accept a template `Engine` parameter (default `mt19937_64`)
with entropy-seeded default, so configurable seeding is built in.

- [x] **randint\<T\>(lo, hi)** — `uniform_int<T>(lo, hi)` (inclusive range)
- [x] **uniform_real\<T\>(lo, hi)** — continuous uniform in [lo, hi)
- [x] **bernoulli(p)** — random bools with P(true) = p
- [x] **normal\<T\>(mean, stddev)** — Gaussian distribution
- [x] **choice\<T\>(container)** — random picks from a set
- [x] **shuffle\<T\>(n)** — reservoir shuffle filter (Fisher-Yates drain)
- [x] **Configurable seeding** (fixed seed for reproducibility vs entropy-seeded)
- [x] **random_bytes(n)** — `random_bytes(chunk_size)` in `random.h`
- [~] **/dev/urandom / /dev/random reader** — trivially composable via
      `io::byte_reader(open("/dev/urandom", O_RDONLY))`

## Go concurrency features

- [x] **csp::dynamic\<T\>** — Dynamic-scoped variables via persistent HAMT.
      Implemented: `dynamic<T>`, `local` (scoped bindings), `imp_local<T>`
      (imp-local), `context`/`context_scope` (transfer over channels).

- [x] **Cancellation and timeouts in dynamic scope** — `cancellation()`,
      `cancellation(duration)`, `cancellation(time_point)` create scoped
      cancel guards (= Go's `WithCancel`, `WithTimeout`, `WithDeadline`).
      `done()` in prialt (= `<-ctx.Done()`), `cancel_reason()` (= `ctx.Err()`),
      `dynamic<T>` (= `ctx.Value()`). Parent→child propagation via dynamic
      scope inherited on `spawn`.

- [x] **select with default** — `csp::none` guard for alt/prialt. Returns
      `INT_MIN` (`constexpr operator int()`), usable as a switch case label.
      `prialt(r >> n, csp::none)` = Go's `select { case ...: default: }`.
      Vector overloads: `alt(ops, csp::none)`, `prialt(ops, csp::none)`.

- [x] ~~**waitgroup**~~ — Subsumed by multi-handle `join()` (range and
      variadic overloads) and shared-writer-death pattern. Channel
      refcounting is the waitgroup count; no manual `Add`/`Done`.

- [x] ~~**errgroup**~~ — Subsumed by imp death interception via dynamic
      scope. Fail-fast (first-error cancellation) is the default behavior
      when no `imp_exit` binding is present. Structured concurrency with
      error collection is just `cancellation()` + `spawn` + `join`.


## Channel topology operations

- [x] **Swap, fuse, split** — Slot-based indirection enables atomic endpoint
      redirection. `swap(a.w, b.w)` exchanges which channels two endpoint
      groups target. The 4-arg `swap(w1, r1, w2, r2)` gives fuse (empty
      middle: create temp channel) and split (valid middle: consumed on
      return). `fuse(w, r)` is shorthand for `swap(w, {}, {}, r)`.

- [x] **Tap** — `tap(w, r)` splices a forwarding imp into a channel,
      returning a `reader<T>` that sees copies of every value. Destroying
      the reader triggers fuse-back via `~tw` vulture and weak endpoint
      references (`weak_writer`/`weak_reader`).

- [x] **Splice** — `splice(w, r, f)` inserts a user-defined filter
      `f(reader<T>, writer<T>)` between `w` and `r`. Auto-fuses back when
      `f` returns via weak refs (same pattern as tap). The filter receives
      copies of the internal endpoints; originals are kept alive until
      after fuse-back.

- [x] **Mid-flight behavior is correct** — `swap_slots` already wakes all
      waiters on both channels with `signal_ = INT_MIN`, causing `prialt` to
      re-resolve each chanop's channel pointer through its slot and re-scan.
      Fuse/split can be applied mid-flight without stale waiters or false
      death signals.

      The two sequential swaps in the 4-arg swap create a brief intermediate
      state under M:N concurrency, but this is benign. The topology is
      momentarily inconsistent (one pair redirected, the other not yet) but
      coherent — every slot points to a valid channel. Concurrent operations
      on the half-swapped topology block until the second swap completes.
      The worst case is a transient stall, never data loss or corruption.

  ### Remaining design questions

  - **Zero-copy fuse**: Could the runtime directly connect two channel
    internals (bypass the forwarding imp)? Waiter wake-and-re-resolve is
    already handled by `swap_slots`, but merging two channel internals
    (waiter lists, lock identity) is a deeper change. Would eliminate the
    extra context switch per message.
  - **Supervision integration**: Supervised endpoints are now handled by
    imp death interception (see Tier C). The dying imp holds its channels
    open while awaiting a restart decision, so no swap/fuse machinery is
    needed for the restart case. Swap/fuse remains useful for mid-flight
    topology changes unrelated to imp death.

## Buffered channels

- [ ] **Built-in channel buffering** — Consider whether `chan<T>` should
      support an optional buffer capacity, as in Go (`make(chan T, n)`) and
      most other CSP/channel implementations.

  ### Current state

  CSP channels are strictly synchronous (rendezvous). Buffering is available
  via the `buffer<T>(n)` part, which spawns an imp with an internal queue.
  This works and composes well, but it means every buffered channel requires
  an extra imp + context switch per message (write → buffer imp → read).

  ### Arguments for built-in buffering

  - **Universal expectation**: Go, Kotlin, Rust (crossbeam/tokio), Clojure
    core.async, and essentially every channel library provides buffered
    channels. Users coming from these will expect `chan<T>(n)`.
  - **Performance**: A buffer integrated into the channel avoids the extra
    imp, context switch, and synchronization overhead. A write to a
    non-full buffered channel completes immediately (no rendezvous needed).
    For high-throughput pipelines this could be significant.
  - **Simpler wiring**: `auto [w, r] = chan<int>(16);` vs
    `auto r = source | buffer<int>(16);` — the former doesn't require
    restructuring the pipeline.
  - **Backpressure semantics are clearer**: A buffered channel has a single
    well-defined backpressure point (buffer full → writer blocks). The
    `buffer` part achieves the same but adds a layer of indirection.

  ### Arguments against (current position)

  - **Synchronous channels are simpler to reason about.** Every send blocks
    until received. No hidden queuing, no capacity surprises. The mental
    model is clean.
  - **An imp can do the same thing.** `buffer<T>(n)` already exists and
    composes with `|`. Adding buffering to the channel itself duplicates
    functionality and complicates the channel implementation (lock-free
    ring buffer, or mutex-guarded queue, capacity checks in the hot path).
  - **Composability**: The `buffer` part can be inserted anywhere in a
    pipeline, combined with other parts (`buffer<T>(n) | throttle<T>(...)`),
    and removed without changing channel types. Built-in buffering is
    fixed at creation.
  - **Implementation complexity**: The channel code is already non-trivial
    (two-phase prialt, lock ordering, M:N thread safety). Adding a ring
    buffer and capacity-aware blocking increases the surface area for bugs.
  - **Synchronous channels enforce design discipline.** Buffering often
    masks backpressure problems rather than solving them. Requiring an
    explicit `buffer` part makes the design choice visible.

  ### Middle ground options

  1. **Small fixed buffer (1-2 slots)**: Just enough to decouple writer and
     reader scheduling without full queue semantics. Minimal implementation
     complexity. Might eliminate the most common reason people reach for
     buffered channels (avoiding unnecessary rendezvous).
  2. **Channel adapter**: A zero-imp buffered wrapper that wraps a `chan<T>`
     with a lock-free ring buffer, presenting the same `writer<T>`/`reader<T>`
     interface. No changes to channel internals. Could be `chan<T>(n)` as
     sugar.
  3. **Do nothing**: Keep the current design. Document that `buffer<T>(n)` is
     the idiomatic way. Accept the imp overhead as the cost of simplicity.

  ### Decision

  Deferred. Revisit after profiling real workloads to determine whether the
  `buffer` part's imp overhead is actually a bottleneck. If it is, option 2
  (adapter) is the least invasive path.

## Runtime improvements

- [x] **mmap-based dynamic stacks** — Implemented as `csp::detail::StackPool`:
      mmap 1 MB regions with guard page, demand paging, MADV_FREE pooling
      (up to 256 cached), `maybe_shrink()` at API boundaries.

- [ ] **High-density stack scaling (future)** — At 100K+ imps,
      per-stack guard pages create ~2 VMAs each, causing kernel memory
      pressure (~400MB at 1M stacks) and Linux `vm.max_map_count` issues.
      Options to explore: (a) single R/W arena with software-only overflow
      detection at API checkpoints (one VMA total, Go-like tradeoff —
      sacrifices hardware guard pages for unlimited scale), (b) hybrid mode
      with guard pages below a threshold and software guards above,
      (c) arena allocation to reduce mmap syscalls. The API checkpoint
      infrastructure from mmap-based stacks (SP checks at channel ops,
      yield, spawn) is the foundation for software-only guards.

- [ ] **Context-aware stack depth analysis** — Revisit the ARM64 instruction
      walker to exploit runtime context available at spawn time: deep closure
      traversal (resolving nested function pointers), ADRP-based vtable and
      global resolution, parameter-driven path pruning (when branch conditions
      depend on values captured in the closure), interprocedural data flow
      (propagating pointer provenance across call boundaries), and
      profile-guided budget calibration. See
      [docs/papers/08-context-aware-stack-analysis.md](papers/08-context-aware-stack-analysis.md)
      for a full discussion of the design space.

- [ ] **Ergonomic I/O wrappers** — Higher-level APIs around the existing
      non-blocking I/O reactor and `csp::blocking` pool. Goals: make common
      I/O patterns (TCP accept loops, line-buffered reads, HTTP requests,
      file reads) feel as natural as channel operations. Candidates:
  - `csp::net::listen(port) → reader<connection>` — accept loop as a stream
  - `csp::net::dial(host, port) → connection` — blocking connect off-loaded
        via `csp::blocking`
  - `csp::io::lines(fd) → reader<string>` — line-buffered reader over
        non-blocking fd, yields per line
  - `csp::io::read_all(fd) → string` — slurp fd via reactor, suspend until
        complete
  - `csp::io::write_all(fd, data)` — non-blocking write with backpressure
  - `csp::file::read(path) → string` / `csp::file::write(path, data)` —
        file I/O off-loaded to blocking pool
  - `csp::http::get(url) → response` / `csp::http::post(url, body)` —
        HTTP client via blocking pool (or non-blocking with reactor)
  - General pattern: blocking syscalls go through `csp::blocking` to avoid
        stalling the imp scheduler; non-blocking I/O integrates with
        the reactor for efficient multiplexing.

- [ ] **Channel-native HTTP server** — An HTTP server designed from the
      ground up around channels rather than callbacks. Rethink conventional
      server design: connections, requests, and responses are all channel
      streams, not handler functions. Key features:

  - **Core model**: `csp::http::serve(port) → reader<request<Req, Resp>>`
        — the server is a stream of typed requests. Each `request<Req, Resp>`
        carries the decoded request body and a `writer<Resp>` for the
        response. Routing is just `where`/`group_by` over the request stream.
  - **Strongly typed JSON codecs**: `request<Req, Resp>` parameterised on
        request/response types. Automatic JSON serialization/deserialization
        (e.g., via nlohmann/json or a reflection-based codec). A route
        binding like `serve<CreateUser, UserResponse>("/api/users", POST)`
        deserializes the body into `CreateUser`, expects a `UserResponse`
        written back. Codec errors become typed error responses automatically.
  - **WebSockets**: `ws::upgrade(req) → chan<ws::message>` — upgrade an
        HTTP request to a bidirectional channel. Reads and writes are just
        channel operations. Backpressure propagates naturally. Multiple
        WebSocket connections composable via merge, fanout, etc.
  - **Server-Sent Events**: `sse::upgrade(req) → writer<sse::event>` —
        upgrade to a write-only event stream. Pipe any `reader<T>` into it
        with `chain`. Client disconnect closes the writer (endpoint death
        observable via alt).
  - **HTTP/2 and h2c**: Native HTTP/2 multiplexing maps naturally to
        channels — each HTTP/2 stream becomes a channel pair. h2c
        (cleartext HTTP/2) for internal service-to-service without TLS
        overhead. Server push as a writer the handler can optionally use.
  - **Middleware as combinators**: Middleware is just stream transforms.
        `map` for header injection, `where` for auth gating, `scan` for
        rate limiting state, `tee` for logging, `batch` for request
        coalescing. Compose with `|` or `chain` rather than wrapping
        handlers.
  - **Request context via dynamic scope**: `csp::dynamic<RequestId>`,
        `csp::dynamic<AuthToken>`, etc. — per-request context propagated
        automatically through spawned imps. No explicit context
        parameter threading. Extractable for cross-imp tracing.
  - **Graceful shutdown**: Close the accept channel → drain in-flight
        requests → waitgroup join. Killswitch for hard timeout. The whole
        lifecycle is observable as channel events.
  - **Design principle**: Every conventional server concept (listener,
        connection pool, request queue, middleware chain, SSE stream, WS
        session) should have a direct channel equivalent. If you'd reach
        for a callback, mutex, or condition variable, there's a channel
        pattern instead.

## Runtime / API

- [ ] **`closer<EP>` type + `done()` / `spawn()` return it** —
      Introduce `closer<EP>`: a wrapper around any endpoint that only
      exposes `operator~` (vulture) and `operator bool` (liveness check).
      No `>>`, no `<<`, no `read()`/`write()` — the type enforces that
      the endpoint is for death observation only.
      ```cpp
      template <typename EP>
      struct closer {
          EP ep;
          auto operator~() const { return ~ep; }
          explicit operator bool() const { return bool(ep); }
      };
      ```
      Instantiated as `closer<reader<>>` (done, spawn handles) or
      `closer<writer<T>>` (observe writer death). Apply to: `done()` →
      `closer<reader<>>`, `spawn(f)` → `closer<reader<std::exception_ptr>>`.
      Users must write `~done()` and `~handle` — the `~` at the call
      site mirrors `case ~0:` in the switch, making the vulture
      convention self-documenting. Bare `done()` in prialt or
      `handle >> exc` won't compile. Discovered via demo 15 hang
      (infinite loop from `case 0:` never matching vulture result `~0`).

- [ ] **Audit main()'s ability to perform CSP operations** — Investigate
      whether `main()` (outside any spawned imp) should be able to use CSP
      operations that require an active imp context, such as `csp::local`
      (dynamic scope bindings), `csp::yield()`, channel reads/writes, etc.
      Currently `csp::local` in `main()` crashes because there is no active
      imp (`g_imp` is null). Determine whether this is by-design or whether
      a lightweight "main imp" context should be established automatically.
      The `fake_clock` demo (19) exposed this: `csp::local` had to be moved
      inside a spawned imp rather than set up in `main()`.

## CI / Build

- [ ] **Local Docker testing for Linux scenarios** — Add a Docker-based
      workflow for testing Linux builds locally, including x86 (amd64)
      cross-compilation. Avoids relying solely on GitHub Actions for
      Linux CI feedback.

## Test cleanup

- [x] **Replace CHECK\_\* macros with plain CHECK()** — 889 occurrences
      replaced across 41 test files.

## Example applications

- [ ] **Design complex example applications** — Build several non-trivial
      examples that demonstrate CSP in realistic system designs. Candidates:

  - [ ] **Chat server** — Multi-room chat with per-client imps,
        fan-out to subscribers, join/leave lifecycle, backpressure on slow
        clients. Demonstrates: fanout, killswitch, dynamic scope (user
        identity), M:N scaling.

  - [ ] **ETL pipeline** — Ingest CSV/JSON, parse, validate, transform,
        enrich (parallel HTTP lookups), deduplicate, batch-write to SQLite.
        Demonstrates: chain, parallel_map, batch, scan, buffer, error
        handling, backpressure propagation.

  - [ ] **Web crawler** — Breadth-first crawl with bounded concurrency,
        URL frontier, dedup via bloom filter, polite per-host rate limiting,
        graceful shutdown. Demonstrates: semaphore, pace, merge, killswitch,
        dynamic scope (crawl config), supervise/retry.

  - [ ] **Sensor fusion dashboard** — Multiple simulated sensor streams
        at different rates, combine_latest for fusion, quantize for display
        throttling, anomaly detection via sliding window, alert fan-out.
        Demonstrates: combine_latest, quantize, slide/nwise, tee, timer,
        dynamic scope (alert thresholds).

  - [ ] **Task scheduler / job queue** — Priority queue of jobs, worker
        pool with steal, per-job timeout, dependency DAG, progress reporting
        over channels, structured cancellation. Demonstrates: balance,
        errgroup, waitgroup, timer, context transfer, killswitch.

  - [ ] **Log aggregator** — Tail multiple log files, parse, route by
        severity, aggregate counts in time windows, flush periodically,
        alert on threshold. Demonstrates: merge, where, group_by, batch,
        timer tick, scan, tee.
