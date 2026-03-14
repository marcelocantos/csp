# The Mutex-to-Channel Transformation: Porting a Go Broker to CSP

## Abstract

We describe a hypothetical port of cworkers — a task broker for
Claude Code agent sessions, written in Go — to the CSP library. The
port serves as a concrete case study for the **mutex-to-channel
transformation**: replacing shared mutable state protected by mutexes
with single-owner imps that communicate via channels. We show how
each component (worker pool, shadow registry, transcript tailer,
connection handler) maps to CSP constructs, how bidirectional death
propagation eliminates explicit cleanup code, and how the dispatch
wait queue simplifies from manual bookkeeping to spawned timeout
imps. We conclude with a line-count analysis that challenges the
assumption that C++ programs are necessarily longer than their Go
equivalents.

## 1. The system being ported

cworkers is a task broker with six subcommands (`serve`, `worker`,
`dispatch`, `shadow`, `unshadow`, `status`) in a single Go binary
(~890 lines). The broker accepts connections over a Unix domain
socket and routes task payloads from dispatchers to pre-spawned
worker agents, with optional context injection from shadowed session
transcripts.

### 1.1 Go architecture

The Go implementation uses three mutex-protected shared structures:

1. **pool** — A `sync.Mutex`-protected `[]taggedWorker` (idle
   workers) and `[]dispatchWaiter` (pending dispatch requests).
   Workers register by adding their connection to the slice.
   Dispatchers take workers by scanning the slice. When no worker
   matches, the dispatcher queues a waiter with a timeout.

2. **shadowRegistry** — A `sync.RWMutex`-protected
   `map[string]*shadow`. Sessions register transcripts; dispatchers
   look up context by session ID.

3. **shadow** — A `sync.RWMutex`-protected `[]string` (rolling
   message window). A goroutine tails the transcript file and adds
   messages. Dispatchers read snapshots.

Each incoming connection spawns a goroutine (`go handleConn(...)`)
that parses the protocol line and calls methods on the shared
structures.

Shutdown uses explicit cleanup: `pool.drain()` closes all
connections and cancels all waiters. `shadow.stop()` closes a
`done` channel. `os.Remove(sock)` cleans up the socket file.

### 1.2 Concurrency characteristics

The system has several concurrency patterns:

- **Producer-consumer with model matching.** Workers produce
  availability; dispatchers consume it. Matching is by model tag
  (e.g., "opus", "sonnet") or wildcard.

- **Queue with timeout.** When no worker is available, dispatchers
  wait up to 30 seconds. The waiter must be cleaned up on timeout.

- **Fan-in.** Multiple connections send commands to shared
  structures concurrently.

- **Background tail.** Each shadow spawns a goroutine that
  continuously reads new lines from a transcript file.

- **Signal-driven shutdown.** SIGINT/SIGTERM triggers graceful
  shutdown of all components.

## 2. The transformation

The core transformation is: **every shared mutable structure becomes
an imp that owns its state, accessed only through channels.** No
mutexes survive.

### 2.1 Pool imp

The Go pool uses a mutex to protect two slices:

```go
type pool struct {
    mu      sync.Mutex
    workers []taggedWorker
    waiters []dispatchWaiter
}
```

Six methods acquire the mutex: `add`, `take`, `wait`, `removeWaiter`,
`count`, `counts`, `drain`. Each method locks, reads/modifies, and
unlocks.

In CSP, the pool is a single imp that owns both slices and
communicates via channels:

```cpp
void pool_imp(
    reader<tagged_worker> registrations,  // workers arrive here
    reader<dispatch_req>  requests,       // dispatchers ask here
    reader<status_req>    status,         // status queries
    reader<>              shutdown        // signal handler
) {
    std::vector<tagged_worker> workers;
    std::vector<dispatch_req> waiters;

    tagged_worker tw;
    dispatch_req  dr;
    status_req    sr;

    for (;;) {
        switch (prialt(~shutdown, registrations >> tw,
                       requests >> dr, status >> sr)) {
        case ~0:
            // Shutdown — death cascade handles the rest
            for (auto& w : workers) w.conn.close();
            for (auto& w : waiters) w.reply << std::nullopt;
            return;

        case 1: {
            // Worker registered — check waiters first
            auto it = find_matching_waiter(waiters, tw.model);
            if (it != waiters.end()) {
                it->reply << tw.conn;
                waiters.erase(it);
            } else {
                workers.push_back(std::move(tw));
            }
            break;
        }

        case 2: {
            // Dispatch request — check pool first
            auto it = find_matching_worker(workers, dr.model);
            if (it != workers.end()) {
                dr.reply << it->conn;
                workers.erase(it);
            } else {
                waiters.push_back(std::move(dr));
                // Spawn timeout imp
                auto reply = dr.reply.copy();
                spawn([reply = std::move(reply), timeout = dr.timeout] {
                    switch (prialt(after(timeout) >> nullptr, ~reply)) {
                        case  0: reply << std::nullopt; break;
                        case ~1: break; // reply already sent
                    }
                });
            }
            break;
        }

        case 3: {
            // Status query
            sr.reply << pool_status{workers, waiters};
            break;
        }
        }
    }
}
```

**What changed.** The mutex disappears. The six methods collapse into
four arms of a single `prialt`. The imp is the synchronisation —
only it can touch `workers` and `waiters`, and it processes one
event at a time.

**Dispatch timeout.** In Go, the dispatch timeout uses
`time.NewTimer` + `select`, plus manual `removeWaiter` cleanup and a
compaction pass over expired waiters in `pool.add()`:

```go
// Go: manual waiter cleanup
func (p *pool) add(conn net.Conn, model string) {
    p.mu.Lock()
    defer p.mu.Unlock()
    // Compact expired waiters and find a match in one pass.
    now := time.Now()
    n := 0
    matched := -1
    for i, w := range p.waiters {
        if now.After(w.expires) {
            continue // drop expired
        }
        // ...
    }
    p.waiters = p.waiters[:n]
    // ...
}
```

In CSP, each waiter spawns a tiny timeout imp. The timeout imp races
`after(timeout)` against the death of the reply channel. If the
timeout fires first, it sends `nullopt` on the reply channel. If
the reply is sent first (worker matched), the reply channel's writer
is dropped, the timeout imp sees `~reply` fire, and exits. No
manual bookkeeping. No expiry compaction. The channel lifecycle
*is* the cleanup.

### 2.2 Shadow registry imp

The Go registry:

```go
type shadowRegistry struct {
    mu       sync.RWMutex
    sessions map[string]*shadow
}
```

The CSP equivalent:

```cpp
struct registry_cmd {
    enum { REGISTER, UNREGISTER, GET, COUNT } op;
    std::string session_id;
    std::string transcript_path;
    int context_lines;
    writer<std::string> reply;
};

void registry_imp(reader<registry_cmd> cmds) {
    std::map<std::string, tail_handle> sessions;

    for (registry_cmd cmd : cmds) {
        switch (cmd.op) {
        case registry_cmd::REGISTER: {
            if (auto it = sessions.find(cmd.session_id);
                it != sessions.end()) {
                it->second.shutdown = {};  // drop writer → kills tail imp
            }
            auto [snap_req_w, snap_req_r] = chan<writer<std::string>>{};
            auto [shutdown_w, shutdown_r] = chan<>{};
            sessions[cmd.session_id] = {
                std::move(snap_req_w), std::move(shutdown_w)};
            spawn([=, snap_req = std::move(snap_req_r),
                      shutdown = std::move(shutdown_r)] {
                tail_imp(cmd.transcript_path, cmd.context_lines,
                         snap_req, shutdown);
            });
            cmd.reply << "OK";
            break;
        }

        case registry_cmd::UNREGISTER:
            sessions.erase(cmd.session_id);
            // Erasing drops the shutdown writer → tail imp sees
            // death → exits → drops its endpoints → cleanup complete
            cmd.reply << "OK";
            break;

        case registry_cmd::GET: {
            auto it = sessions.find(cmd.session_id);
            if (it == sessions.end()) {
                cmd.reply << "";
                break;
            }
            // Request snapshot from tail imp
            auto [snap_w, snap_r] = chan<std::string>{};
            it->second.snap_req << std::move(snap_w);
            cmd.reply << snap_r.read();
            break;
        }

        case registry_cmd::COUNT:
            cmd.reply << std::to_string(sessions.size());
            break;
        }
    }
}
```

**What changed.** The `RWMutex` disappears. The distinction between
read-lock (`get`, `count`) and write-lock (`register`, `unregister`)
is irrelevant — the imp processes commands sequentially. The `stop()`
method on shadow disappears — unregistration just drops the shutdown
writer, and death cascades.

### 2.3 Tail imp

The Go shadow goroutine uses `select` with `time.After` for polling
and a `done` channel for shutdown:

```go
select {
case <-s.done:
    return
case <-time.After(500 * time.Millisecond):
}
```

The CSP tail imp uses `prialt` with `tick()` and serves snapshot
requests:

```cpp
void tail_imp(std::string path, int max_lines,
              reader<writer<std::string>> snap_requests,
              reader<> shutdown) {
    auto f = open_file(path);
    std::deque<std::string> messages;

    writer<std::string> snap_reply;

    for (;;) {
        // Read all available lines
        while (auto line = scan_line(f))
            process_line(*line, messages, max_lines);

        // Wait for: poll tick, snapshot request, or shutdown
        switch (prialt(~shutdown,
                       snap_requests >> snap_reply,
                       tick(500ms) >> nullptr)) {
        case ~0: return;                        // shutdown
        case  1:                                // snapshot request
            snap_reply << format_snapshot(messages);
            break;
        case  2: break;                         // poll tick — loop back
        }
    }
}
```

**What changed.** The `sync.RWMutex` on the message slice disappears.
The tail imp owns the messages exclusively. Snapshot requests arrive
as channels — the registry sends a reply writer, the tail imp writes
the snapshot to it. No lock contention between the tailer and
snapshot readers.

The Go version has a subtle design: `shadow.add()` takes a write
lock, `shadow.snapshot()` takes a read lock. Under high tail
frequency, the write lock can starve snapshot readers. The CSP
version cannot starve because the `prialt` interleaves snapshot
requests with poll ticks fairly.

### 2.4 Connection handler

The Go version spawns a goroutine per connection and calls methods
on shared structures:

```go
go handleConn(conn, p, reg, dispatchWait)
```

The CSP version spawns an imp per connection and sends on channels:

```cpp
void handle_conn(socket conn,
                 writer<tagged_worker> pool_reg,
                 writer<dispatch_req> pool_dispatch,
                 writer<registry_cmd> shadow_cmds) {
    auto line = conn.read_line();
    auto [cmd, args] = parse_command(line);

    switch (cmd) {
    case WORKER:
        pool_reg << tagged_worker{std::move(conn), args.model};
        break;

    case DISPATCH: {
        auto task = conn.read_all(max_payload);
        auto payload = build_payload(task, args.session, shadow_cmds);

        auto [reply_w, reply_r] = chan<std::optional<socket>>{};
        pool_dispatch << dispatch_req{
            args.model, std::move(reply_w), 30s};

        if (auto worker = reply_r.read(); worker) {
            worker->write(payload);
            conn.write("OK\n");
        } else {
            conn.write("NO_WORKERS\n");
        }
        break;
    }

    case SHADOW:
        shadow_cmds << registry_cmd{
            registry_cmd::REGISTER,
            args.session, args.transcript, args.context_lines,
            /* reply */ {}};
        conn.write("OK\n");
        break;

    // UNSHADOW, STATUS similar
    }
}
```

**What changed.** The function signature changes from shared
references (`*pool`, `*shadowRegistry`) to channel endpoints. The
handler cannot access pool or registry state directly — it can only
send requests and wait for replies. This is the mutex-to-channel
transformation at the API boundary.

### 2.5 Server top-level

```cpp
void serve(std::string sock_path, duration dispatch_wait) {
    auto listener = unix_listen(sock_path);
    chmod(sock_path, 0700);

    // Create the channel graph
    auto [pool_reg_w, pool_reg_r]     = chan<tagged_worker>{};
    auto [pool_req_w, pool_req_r]     = chan<dispatch_req>{};
    auto [shadow_w, shadow_r]         = chan<registry_cmd>{};
    auto [status_w, status_r]         = chan<status_req>{};
    auto [shutdown_w, shutdown_r]     = chan<>{};

    // Spawn service imps
    spawn([=] { pool_imp(pool_reg_r, pool_req_r,
                         status_r, shutdown_r); });
    spawn([=] { registry_imp(shadow_r); });

    // Signal handling
    spawn([=] {
        signal_channel(SIGINT, SIGTERM).read();
        shutdown_w = {};   // drop → pool sees death → drains
        listener.close();  // breaks accept loop
    });

    // Accept loop
    for (auto conn : listener.accept_loop()) {
        spawn([=, conn = std::move(conn)] {
            handle_conn(conn,
                        pool_reg_w.copy(),
                        pool_req_w.copy(),
                        shadow_w.copy());
        });
    }
}
```

**What changed.** The channel graph is explicit — all communication
pathways are visible in the wiring. In the Go version, shared
references are passed to `handleConn` and the communication topology
is implicit in which methods each goroutine calls.

## 3. The shutdown cascade

Shutdown is where the transformation produces the most dramatic
simplification.

### 3.1 Go shutdown

The Go version requires explicit cleanup across multiple components:

```go
// Signal handler
go func() {
    <-sig
    ln.Close()  // break accept loop
}()

// After accept loop exits:
p.drain()       // close all worker conns, cancel all waiters
os.Remove(sock) // clean up socket file
```

And `pool.drain()` must manually iterate:

```go
func (p *pool) drain() {
    p.mu.Lock()
    defer p.mu.Unlock()
    for _, w := range p.workers {
        w.conn.Close()
    }
    p.workers = nil
    for _, w := range p.waiters {
        close(w.ch)
    }
    p.waiters = nil
}
```

And each shadow must be stopped:

```go
func (s *shadow) stop() {
    select {
    case <-s.done:
    default:
        close(s.done)
    }
}
```

Total: ~25 lines of explicit cleanup spread across three types.

### 3.2 CSP shutdown

```cpp
shutdown_w = {};  // Drop the shutdown writer. That's it.
```

One line. Here is what happens:

1. The shutdown writer is dropped.
2. The pool imp's `prialt` includes `~shutdown`. The vulture fires.
3. The pool imp closes worker connections, sends `nullopt` to
   waiters, and returns.
4. The pool imp's return drops its channel endpoints (`pool_reg_r`,
   `pool_req_r`, `status_r`).
5. Connection handler imps waiting to send on those channels observe
   broken-pipe death and exit.
6. The registry imp's reader (`shadow_r`) dies when all
   `shadow_w.copy()` aliases are dropped (by exiting handler imps).
7. The registry imp exits its `for` loop, dropping all `tail_handle`
   entries.
8. Each tail handle's shutdown writer is dropped, cascading death to
   tail imps.
9. All imps have exited. All resources are freed by RAII.

This is cleanup completeness (Paper 13, §3.3) in action. The
process graph is a DAG (signal handler → pool → handlers → registry
→ tail imps), so death cascades cleanly from root to leaves.

The Go version requires the developer to manually implement each
step. The CSP version requires the developer to wire up the channel
graph correctly — after that, shutdown is automatic.

## 4. Side-by-side comparison

| Aspect | Go (current) | CSP (ported) |
|--------|-------------|--------------|
| Pool synchronisation | `sync.Mutex` — 6 methods | Channel ownership — one `prialt` loop |
| Shadow registry | `sync.RWMutex` + map | Registry imp owns the map |
| Shadow rolling window | `sync.RWMutex` + slice | Tail imp owns the slice |
| Dispatch wait/timeout | `time.NewTimer` + `select` + manual `removeWaiter` + expiry compaction | `spawn` timeout imp; channel death is cleanup |
| Signal handling | `signal.Notify` + goroutine | `signal_channel()` — channel-native |
| Shutdown | Explicit `drain()`, `stop()`, `Close()` — ~25 lines | Drop one writer — death cascades |
| Worker stale detection | Write error → retry loop | Writer death visible in `prialt` |
| Topology | Implicit in shared references | Explicit in channel graph wiring |
| Race safety | Correct by mutex discipline (reviewer must verify) | Correct by construction (A1 + A2) |

## 5. Line count analysis

The claim that a C++ port would be "2-3x the code" is worth
examining carefully. It turns out to be wrong.

### 5.1 Where C++ genuinely adds lines

**String handling.** Go's `strings.SplitN`, `strings.Fields`,
`strings.TrimSpace`, `fmt.Sprintf` are each one-liners. C++
equivalents require either `std::istringstream` gymnastics, manual
`find`/`substr` loops, or a string utility library. The cworkers
protocol parser is ~50 lines of Go string manipulation. A C++
equivalent would be ~70-80 lines.

**JSON parsing.** Go's `encoding/json` provides `json.Unmarshal`
with struct tags — a single call populates a typed struct. C++ needs
a library (nlohmann/json, rapidjson) with more verbose accessor
patterns. The transcript parsing (~60 lines Go) would be ~80-90
lines C++.

**Error handling.** Go's `if err != nil { return }` is terse. C++
without exceptions needs `std::expected`, `std::optional`, or
explicit error codes. With exceptions, error handling is comparable
or shorter, but the error paths are implicit.

**Socket I/O.** Go's `net.Dial`, `net.Listen`, `bufio.NewReader` are
high-level. Without CSP's I/O wrappers, raw POSIX socket code is
~1.5x longer. With CSP's planned I/O layer (`csp::net::listen`,
`csp::net::dial`), it would be comparable.

### 5.2 Where CSP is shorter

**Concurrency primitives.** The pool's 6 mutex-protected methods
(~100 lines Go) collapse into a single `prialt` loop (~50 lines
C++). The shadow registry's 4 methods (~40 lines Go) become a
registry imp (~40 lines C++). The shadow's mutex-protected add and
snapshot (~30 lines Go) become part of the tail imp (~25 lines C++).

**Shutdown.** ~25 lines of explicit cleanup (Go) become ~1 line
(CSP).

**Dispatch timeout.** ~30 lines of manual waiter management (Go)
become ~8 lines (spawned timeout imp).

### 5.3 Net estimate

| Category | Go lines | C++ lines | Ratio |
|----------|----------|-----------|-------|
| CLI parsing, usage | ~130 | ~150 | 1.15x |
| Protocol parsing | ~50 | ~75 | 1.5x |
| JSON parsing | ~60 | ~85 | 1.4x |
| Socket I/O | ~80 | ~100 | 1.25x |
| Concurrency (pool, registry, shadow) | ~250 | ~150 | 0.6x |
| Shutdown/cleanup | ~25 | ~5 | 0.2x |
| Server wiring | ~40 | ~30 | 0.75x |
| Client commands (worker, dispatch, shadow, status) | ~150 | ~170 | 1.1x |
| Includes, types, boilerplate | ~30 | ~80 | 2.7x |
| **Total** | **~815** | **~845** | **1.04x** |

The C++ port would be approximately the **same length** as the Go
version. The string/JSON/boilerplate overhead is almost exactly
offset by the concurrency simplification.

The "2-3x" estimate reflected a generic "C++ is verbose" assumption
rather than an analysis of this specific program. For systems where
the concurrency logic is a significant fraction of the codebase —
and cworkers is ~30% concurrency by line count — CSP's channel model
absorbs enough complexity to neutralise C++'s verbosity overhead.

### 5.4 When C++ would be longer

For programs where concurrency is a small fraction (e.g., a CLI tool
that does string processing with one background goroutine), the
string/JSON/boilerplate overhead dominates and C++ would indeed be
1.5-2x longer. The break-even point is roughly when concurrency
logic exceeds ~20% of the codebase.

### 5.5 When C++ would be shorter

For programs with complex shutdown choreography (graceful
degradation, partial restart, health checking), the death-cascade
model can save hundreds of lines. A system with 10 interacting
services, each with explicit shutdown logic, might have 200+ lines
of Go cleanup code that reduces to 10 lines of channel graph wiring
in CSP.

## 6. Lessons from the transformation

### 6.1 Mutexes encode implicit protocols

Each mutex-protected method in the Go pool encodes an implicit
protocol: "acquire lock, check state, modify state, release lock."
The protocol is correct only if every method follows it and no
method forgets to acquire the lock.

The CSP pool imp makes the protocol explicit: events arrive on
channels, and the `prialt` processes them one at a time. The
protocol is correct by construction — there is no lock to forget.

### 6.2 Timeout cleanup is a lifecycle problem

The Go dispatch timeout requires manual bookkeeping: register a
waiter, start a timer, on timeout remove the waiter, compact the
waiter list periodically. The CSP version recognises that timeout
cleanup is a **channel lifecycle** problem: when the timeout fires,
the reply channel dies, and everyone observing it reacts
automatically.

This is a general principle: whenever cleanup logic exists because
"a thing might go away," CSP's death propagation can replace it.

### 6.3 Explicit topology is a feature

The Go version passes `*pool` and `*shadowRegistry` to
`handleConn`. Any goroutine with a reference can call any method.
The communication topology is implicit.

The CSP version passes channel endpoints. A handler imp can only
send registrations or requests — it cannot, for example, directly
inspect the pool's internal state. The topology is explicit,
documented, and checkable (Paper 12, §4).

This is not just an aesthetic improvement. It means the pool imp's
invariants can be verified without knowing what handler imps do. The
handlers are black boxes that produce events on known channels. The
pool imp processes those events. The verification is compositional
(Paper 13, §6).

### 6.4 The transformation is mechanical

The mutex-to-channel transformation follows a recipe:

1. For each mutex-protected struct, create a channel for each
   method that callers invoke.
2. Create an imp that owns the struct's state and runs a `prialt`
   loop over all channels.
3. Each `prialt` arm implements the logic of the corresponding
   method.
4. Methods that return values use a reply channel (send a reply
   writer, receive on it).
5. For cleanup/shutdown, add a `~shutdown` vulture arm.

This recipe is general. It applies to any mutex-protected shared
state in any Go (or C++, or Java) program. The resulting CSP code is
no longer than the original and is provably race-free by
construction.
