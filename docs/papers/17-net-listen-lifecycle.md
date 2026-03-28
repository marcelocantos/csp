# net::listen Accept Loop Lifecycle — Investigation Plan

## Status

The `net::listen` API works functionally (echo test passes data correctly)
but has two lifecycle bugs:

1. **HAMT leak (SOLVED)**: Shared `cancel_guard` across imps causes
   `csp::local` to restore in the wrong imp's dynamic scope. TLA+ spec
   `formal/ListenLifecycle.tla` confirms. Fix: stopper-imp pattern where
   producer owns the guard exclusively.

2. **fcontext terminate (OPEN)**: `std::terminate` with no current
   exception, backtrace shows `start() → finish` (fcontext trampoline).
   Happens during `schedule()`, not static destruction. The echo test
   triggers it; listen-and-drop does not.

## What we know

- The terminate happens when the stopper imp cancels the producer's
  cancel_guard, causing `io::accept` → `wait_readable` to throw
  `canceled`. The producer catches it. Then `do_switch(Status::exit)`
  runs. Something goes wrong during the exit, and `start()` falls
  through to fcontext's `finish` label.

- The `this == self` check in `run()` does NOT fire (confirmed with
  fprintf). So `do_switch` successfully finds a target and calls
  `target->run(Status::exit)`.

- `std::terminate` is called with no current exception. This rules out
  uncaught exception propagation. It points to either:
  - A `noexcept` function trying to throw
  - A thread destructor on a joinable thread
  - `std::terminate` called explicitly

- The listen-and-drop case (no connections) exits cleanly with W=0 R=0.
  The crash only happens when a connection was accepted and the
  byte_reader/byte_writer pipeline exists.

## Investigation plan

### Step 1: Reproduce with single-threaded mode

Run with `set_maxprocs(1)` to eliminate M:N scheduling complexity.
If the crash disappears, it's a threading/migration issue. If it
persists, it's a single-threaded lifecycle issue.

```cpp
csp::set_maxprocs(1);
spawn([...]);
schedule();
```

### Step 2: Instrument do_switch and start

Add fprintf to every branch in `do_switch` and `start`:

- Before `do_switch` calls `target->run(status)`
- After `switch_to` returns in `run()`
- In `destroy_imp`
- After the try/catch in `start()`, before `do_switch(Status::exit)`
- A canary after `do_switch(Status::exit)` in `start()` (should never print)

This will show the exact sequence of imp exits and which imp hits
the finish label.

### Step 3: Check if canceled propagates through do_switch

The cancel scope is active when `do_switch(Status::exit)` runs.
`do_switch` acquires `current_p().run_mu` via `lock_guard`. Mutex
acquisition doesn't throw. But does `run()` itself check cancellation
anywhere?

Check: does `switch_to` (specifically `jump_fcontext`) interact with
the cancel scope? The cancel scope works through `done()` in prialt —
`switch_to` doesn't use prialt. So cancellation shouldn't affect
the exit path.

But: what if the target imp (that we switch to) is ALSO in a canceled
state? When `target->run(Status::exit)` switches to target, the
target resumes from its `switch_to` return point. If the target was
blocked in `wait_readable` with `prialt(done(), ~signal)`, it sees
`done()` fire, throws `canceled`. The `canceled` propagates through
`start_f` into the catch block. Then `do_switch(Status::exit)` runs.
This is the expected path — it should work.

Unless: the target imp's `start_f` has ALREADY returned (the imp
exited normally), and `do_switch(Status::exit)` is running from
the imp's `start()`. Then `do_switch` tries to switch to p.main.
p.main resumes in `worker_loop` → `next->run()` return path →
processes `killyou`. This is also expected.

### Step 4: Count the imps and trace their exits

The echo test creates these imps:
1. Outer spawn (creates listener, spawns server + client)
2. Producer (accept loop, from `spawn_producer`)
3. Sentinel (watches ~out_copy)
4. Stopper (watches ~stop_r, calls guard())
5. Server (accepts one connection, echoes)
6. Client (dials, sends, receives)
7. byte_reader (for server's connection input)
8. byte_writer (for server's connection output)
9. byte_reader (for client's connection input)
10. byte_writer (for client's connection output)

Plus spawn handles create channels but the spawn handle readers are
discarded immediately.

Trace each imp's exit to see which one hits the finish label.

### Step 5: Check the byte_reader/byte_writer exit path

byte_reader exits when `io::read` returns 0 (EOF) or -1 (error).
byte_writer exits when the input channel dies or `io::write` fails.

When the client drops `conn.output` (line: `conn.output = {}`), the
byte_writer's input channel reader dies. byte_writer exits.
When the client drops `conn.input` (line: `conn.input = {}`), the
byte_reader's output channel writer dies. byte_reader exits.

But byte_reader is inside `io::read` → `wait_readable`. If the fd
is closed (byte_writer closed its dup'd fd), `wait_readable` may
throw or return an error. Check: does closing one dup'd fd affect
the other? On Unix, `dup` creates an independent fd — closing one
doesn't affect the other. But `shutdown(fd, SHUT_WR)` on the
original socket affects both.

This is a potential issue: byte_writer closes its dup'd fd via
`io::close(wfd)`. But `wfd` is a dup of the original socket. Closing
`wfd` doesn't close the underlying socket — the original fd (`rfd`)
still holds a reference. byte_reader's `rfd` remains valid.

### Step 6: Check if the terminate is from the Reactor

The reactor has an internal thread. When the reactor singleton is
destroyed during static destruction, `~Reactor()` calls `shutdown()`.
If `shutdown()` is called while a reactor event callback is in
progress, the join might deadlock or the callback might access freed
memory.

Check: is the reactor still running when the terminate happens?
Add a fprintf to `Reactor::shutdown()` and `Reactor::loop()` exit.

### Step 7: Write a minimal reproducer

Strip the test down to the absolute minimum that triggers the
terminate. Start with:

```cpp
spawn([] {
    auto guard = cancellation();
    chan<> stop;
    spawn([stop_w = std::move(stop.w)] {
        // immediately drop stop_w
    });
    auto stop_r = std::move(stop.r);
    spawn([stop_r = std::move(stop_r), &guard] {
        prialt(~stop_r);
        guard();
    });
    try {
        io::accept(listen_fd, ...);
    } catch (canceled) {}
});
```

If this crashes without any network I/O (just the cancel/stopper
pattern), the bug is in the cancel-during-exit interaction, not in
the network code.

## Likely root cause hypothesis

The stopper imp calls `guard()` which fires `done()` for all imps
in the cancel scope. The producer catches `canceled`. But the stopper
and producer are on the same processor. When the stopper exits via
`do_switch(Status::exit)`, it switches to the producer (now runnable).
The producer catches `canceled` and exits via `do_switch(Status::exit)`.
The producer switches to p.main. p.main processes the killyou chain.

But there might be a third imp (byte_reader or byte_writer) also in
the cancel scope, also woken by `done()`. If that imp throws
`canceled` while being switched to during the killyou chain, the
exception propagates through `start()` past the try/catch (because
the imp already returned from `start_f` and is in `do_switch`).

This would explain why the crash only happens with connections (which
create byte_reader/byte_writer imps) and not with listen-and-drop.

## Files to read

- `src/csp.cc`: `start()` (line 344), `do_switch()` (line 290),
  `Imp::run()` (line 216), `destroy_imp()` (line 202)
- `src/net.cc`: `listen()` accept loop (line 129)
- `src/io.cc`: `wait_readable` (line 14), `accept` (line 95)
- `src/cancel.cc`: `cancel_state`, `cancel_guard`, `done()` (line 140)
- `include/csp/part/io.h`: `byte_reader`, `byte_writer`
