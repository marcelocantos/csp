# Combinator TODO

## Tier C — Coordination and resilience

- [ ] **Supervision trees** — Imp lifecycle management with automatic restart,
      structured shutdown, and failure escalation. Not a stream combinator —
      a runtime construct that owns a set of imps and their restart policies.

  ### Design overview

  A **supervisor** is an imp that monitors child imps via their
  `reader<exception_ptr>` join handles (returned by `spawn`). When a child
  dies, the supervisor applies a restart strategy. Supervisors are themselves
  supervisable, forming a tree.

  ### Core types

  ```cpp
  namespace csp {

  // Restart strategies (what to do when a child dies).
  struct one_for_one {};   // Restart only the failed child.
  struct one_for_all {};   // Restart all children when any fails.
  struct rest_for_one {};  // Restart the failed child + all later siblings.

  using restart_strategy = std::variant<one_for_one, one_for_all, rest_for_one>;

  // Restart intensity limits.
  struct restart_policy {
      restart_strategy strategy = one_for_one{};
      int max_restarts = 3;               // Within the window.
      csp::duration window = 5s;   // Rolling window.
      csp::duration backoff = 0s;  // Delay before restart (0 = immediate).
      // If max_restarts exceeded within window, supervisor itself dies
      // (escalates to its parent supervisor).
  };

  // Child specification.
  struct child_spec {
      std::string id;                // Unique name within this supervisor.
      std::function<void()> start;   // Factory — called on each (re)start.
      bool transient = false;        // true: don't restart on normal exit.
  };

  class supervisor;
  }
  ```

  ### Supervisor API

  ```cpp
  namespace csp {

  class supervisor {
  public:
      explicit supervisor(restart_policy policy = {});

      // Register a child. Returns *this for chaining.
      supervisor& add(std::string id, std::function<void()> f);
      supervisor& add(child_spec spec);

      // Start all children and run the supervision loop.
      // Blocks until shutdown or escalation (max_restarts exceeded).
      // Throws the escalating exception if it exits due to failure.
      void run();

      // Request graceful shutdown: drop all children's killswitch channels,
      // wait for them to exit (with optional timeout).
      void shutdown(csp::duration timeout = 5s);

      // Dynamic child management (from within supervised imps).
      void add_child(child_spec spec);     // Hot-add.
      void remove_child(std::string id);   // Graceful stop + remove.
  };

  }
  ```

  ### Supervision loop

  The supervisor's `run()` method:
  1. Spawns all registered children, collecting join handles.
  2. Enters an `alt` loop over all join handles + an optional shutdown
     channel.
  3. When a join handle fires:
     - If `exception_ptr` is null → normal exit. If child is `transient`,
       don't restart. Otherwise restart per strategy.
     - If `exception_ptr` is non-null → abnormal exit. Log, increment
       restart counter, apply strategy.
  4. Restart counter uses a sliding window: restarts older than `window`
     are forgotten. If counter exceeds `max_restarts`, supervisor itself
     throws (escalation).
  5. On shutdown signal: drop all children's killswitch writers (if wired),
     then `alt` over remaining join handles with a timeout. Force-drop any
     stragglers.

  ### Strategy semantics

  - **one_for_one**: Only the failed child is restarted. Other children
    are unaffected. Simplest, most isolated.
  - **one_for_all**: All children are stopped (in reverse start order) and
    restarted (in start order). For tightly coupled children where one
    failure invalidates the others' state.
  - **rest_for_one**: The failed child and all children started after it
    are stopped and restarted. For sequential dependencies (B depends on A,
    C depends on B).

  Stopping a child: drop its killswitch writer (if wired via `child_spec`),
  wait briefly, then drop its reader (severing the join handle). The child
  detects endpoint death via alt and exits.

  ### Cancellation integration

  Children receive cancellation via endpoint death, not signals. Two patterns:

  1. **Killswitch** (preferred): Supervisor holds a `writer<>` per child.
     Child's top-level loop includes `~killswitch_reader` in its alt.
     Supervisor drops the writer to request shutdown.

  2. **Input channel death**: If the child's work is driven by a reader
     that the supervisor controls, dropping the writer end cancels it
     naturally.

  ### Escalation

  When a supervisor exceeds its restart limit, it throws the last child's
  exception. If this supervisor is itself a child of a parent supervisor,
  the parent sees the death and applies its own strategy. This creates
  the tree structure: leaf imps are supervised by mid-level supervisors,
  which are supervised by a root supervisor.

  ```
  root_supervisor (one_for_one, max=5)
  ├── db_supervisor (one_for_all, max=3)
  │   ├── connection_pool
  │   └── migration_worker
  ├── web_supervisor (one_for_one, max=10)
  │   ├── accept_loop
  │   └── request_handler_pool
  └── background_supervisor (rest_for_one, max=3)
      ├── scheduler
      └── worker_pool
  ```

  ### What CSP already provides

  - `spawn()` → `reader<exception_ptr>`: natural death notification
  - `alt` over join handles: supervisor select loop
  - Endpoint death propagation: cancellation for free
  - `after()`: restart delays, window timers
  - `dynamic<T>`: propagate supervisor context to children

  ### What needs building

  - `supervisor` class with child registry and restart bookkeeping
  - Sliding-window restart counter
  - Strategy dispatch (one_for_one / one_for_all / rest_for_one)
  - Ordered shutdown with timeout
  - Optional: dynamic child add/remove (hot management)
  - Optional: `supervisor_spec` for declarative tree construction

  ### Relationship to stream combinators

  A **`retry`** part (stream-level restart) is a thin layer on top:
  ```cpp
  template <typename T>
  auto retry(std::function<reader<T>()> factory, restart_policy policy);
  // Returns reader<T>. On factory's reader death, re-invoke factory
  // per policy. Output is seamless concatenation of retried streams.
  ```

  This is the stream-shaped surface of supervision, useful as a combinator.
  The full `supervisor` class handles the non-stream case (arbitrary imps,
  not just producers).

- [ ] **circuit_breaker** `(reader<T>, config) → reader<T>` — Trips after
      threshold failures, half-open probe, auto-reset. Can compose with
      supervision: supervisor detects circuit-breaker escalation as a child
      failure.

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

- [x] **select with default** — `csp::none` guard for alt/prialt. Returns
      `INT_MIN` (`constexpr operator int()`), usable as a switch case label.
      `prialt(r >> n, csp::none)` = Go's `select { case ...: default: }`.
      Vector overloads: `alt(ops, csp::none)`, `prialt(ops, csp::none)`.

- [ ] **waitgroup** — Explicit join point for a dynamic set of imps.
      `schedule()` is all-or-nothing; waitgroup allows partial joins.

- [ ] **errgroup** — Structured concurrency with first-error cancellation
      and error collection. Spawns N imps, cancels all on first
      failure, returns the error.

- [ ] **singleflight** — Deduplicate concurrent calls to the same key.
      Only one executes; others wait for the result.

- [ ] **semaphore** — Bounded concurrency: limit N concurrent imps
      past a point.

## Channel topology operations

- [ ] **Channel fusing, cutting, and splicing** — Investigate primitives for
      dynamically rewiring channel topology at runtime. Currently, channel
      endpoints are fixed at creation — once a writer and reader are connected,
      that connection is permanent until one side dies. This is limiting for
      supervision trees (restart rewiring), live reconfiguration, and hot
      code reload.

  ### Fuse
  Connect two previously independent channels by splicing a reader to a
  writer. Conceptually: given `reader<T> r` and `writer<T> w` on different
  channels, create a forwarding imp that drains r into w. This already works
  via `spawn(r.stream_to(std::move(w)))` — but a first-class fuse could
  be zero-copy (directly connect the channel internals) rather than requiring
  an intermediate imp and double synchronization.

  ### Cut
  Sever an existing channel connection, yielding two independent halves.
  Given a live channel, produce a new `writer<T>` that the reader now sees,
  and a new `reader<T>` that the writer now feeds. The original endpoints
  remain valid but are now on separate channels. Use cases:
  - Insert a filter into a live pipeline (cut, then fuse through the filter)
  - Supervision restart: cut the old child's channels, fuse to the new child

  ### Splice
  Compound operation: cut a channel and insert a new stage (filter, buffer,
  monitor) between the halves. Equivalent to cut + fuse + fuse but could be
  atomic to avoid message loss during the transition.

  ### Design questions

  - **Atomicity**: Can fuse/cut/splice be atomic with respect to in-flight
    messages? A naïve cut might lose a message that's mid-transfer. May need
    a "drain then cut" protocol, or accept that cut is only safe at quiescent
    points.
  - **Zero-copy fuse**: Could the runtime directly connect two channel
    internals (bypass the forwarding imp)? This would require channels to
    support re-parenting of waiters. Complex but eliminates the extra
    context switch per message.
  - **Supervision integration**: Supervised endpoints (from the supervision
    tree design) are essentially auto-fuse-on-restart. A cut/fuse primitive
    would be the mechanism the supervisor uses internally.
  - **Relationship to proxy channels**: A "supervised endpoint" is a
    persistent outer channel fused to a series of inner channels over time
    (one per child lifetime). Each restart is a cut of the old inner + fuse
    to the new inner.
  - **Hot pipeline reconfiguration**: With splice, you could insert a
    `metrics` tap, `buffer`, or `throttle` into a running pipeline without
    restarting it. This is powerful for operational debugging.
  - **Type safety**: Fuse and splice need type compatibility at the join
    point. Cut preserves the type trivially.

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
