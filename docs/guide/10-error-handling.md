# Error Handling

Every `spawn()` call returns a `reader<std::exception_ptr>` -- an exception
channel tied to the new imp. If the imp's function throws, the
exception is captured and sent on this channel rather than crashing the
program. This design makes error propagation explicit and composable.

## The exception channel

```cpp
reader<std::exception_ptr> ex = spawn([] {
    // ... work ...
    throw std::runtime_error("oops");
});
```

The returned reader delivers at most one message: the exception that
terminated the imp. If the imp returns normally, the writer
side closes without sending and the reader sees EOF.

## join() -- wait and rethrow

`join()` reads from the exception channel. If the imp threw, `join()`
rethrows the original exception in the caller's context:

```cpp
auto ex = spawn([] {
    throw std::runtime_error("failed");
});

try {
    join(ex);           // blocks until the MT finishes
} catch (std::exception const & e) {
    // e.what() == "failed"
}
```

If the imp completed normally, `join()` returns immediately (the
exception channel is closed, so the read sees EOF and there is nothing to
rethrow).

The pattern `join(spawn([]{...}))` is the simplest way to run work on a
imp and propagate any errors back to the caller:

```cpp
join(spawn([] {
    do_critical_work();  // any exception propagates to the join() call
}));
```

## Fire-and-forget and global_exception_handler

When you discard the returned `reader<std::exception_ptr>`, exceptions from
that imp are sent to `global_exception_handler` instead. This is a
global `writer<std::exception_ptr>` that you can reassign:

```cpp
// Default: writes to a dead channel (exceptions silently discarded).
// Reassign to log or handle uncaught MT exceptions:
global_exception_handler = spawn_consumer<std::exception_ptr>([](auto r) {
    for (std::exception_ptr ep; r >> ep;) {
        try {
            std::rethrow_exception(ep);
        } catch (std::exception const & e) {
            std::cerr << "unhandled MT exception: " << e.what() << "\n";
        }
    }
});
```

If the exception channel write also fails (because neither the returned reader
nor `global_exception_handler` has a live reader), the program calls
`std::terminate()`. This is a last-resort safety net -- it means an exception
was thrown and every path to observe it was closed.

The escalation chain is:

1. Send exception to the per-MT channel (the `reader<std::exception_ptr>`
   returned by `spawn()`).
2. If that channel is dead, send to `global_exception_handler`.
3. If that also fails, call `std::terminate()`.

## range\<T\> -- iterator-friendly error propagation

`spawn_range<T>()` returns a `range<T>` that wraps both a data channel and the
imp's exception channel. Iteration reads from the data channel; when
the data channel closes, the range checks the exception channel and rethrows
any captured exception:

```cpp
auto r = spawn_range<int>([](writer<int> w) {
    for (int i = 0; i < 100; ++i) {
        w << i;
        if (i == 50)
            throw std::runtime_error("producer failed");
    }
});

try {
    for (int n : r) {
        process(n);     // processes 0..50
    }
} catch (std::runtime_error const & e) {
    // e.what() == "producer failed"
}
```

This is the recommended pattern for producer-consumer pipelines where the
consumer must know whether the producer finished cleanly or failed.

## Pipeline error propagation

Combinators like `map`, `where`, `scan`, and others run inside spawned
imps. When a combinator throws:

1. The combinator's output channel closes (its writer is destroyed).
2. Downstream readers see EOF and exit their loops naturally.
3. Upstream writers see a dead reader on their next write attempt and exit.

Death propagates in both directions through the pipeline without any special
error-handling code:

```cpp
auto r = csp::part::map<int>([](int n) -> int {
    if (n < 0) throw std::domain_error("negative");
    return n * 2;
}).spawn(std::move(source));

// Iteration stops at the first negative value.
// The map MT's exception goes to global_exception_handler
// (unless you captured the spawn's reader).
for (int n : r) {
    process(n);
}
```

If you need to catch the combinator's exception, use `spawn_range` or
explicitly `join()` the underlying imp.

## try\_map -- resilient pipelines

Sometimes you want a pipeline to *survive* errors rather than tear down on the
first exception. `try_map` catches exceptions from the mapping function and
routes them to a side channel as `std::exception_ptr`, while successful results
continue through the pipeline normally:

```cpp
auto src = chan<int>();
auto err = chan<std::exception_ptr>();

auto out = std::move(src.r) | try_map<int>([](int n) -> int {
    if (n < 0) throw std::domain_error("negative");
    return n * 2;
}, std::move(err.w));

// Producer
spawn([w = std::move(src.w)] {
    w << 1; w << -1; w << 2; w << -2; w << 3;
});

// Error handler -- runs concurrently
spawn([r = std::move(err.r)] {
    for (std::exception_ptr ep; r >> ep;) {
        try { std::rethrow_exception(ep); }
        catch (std::exception const& e) {
            fprintf(stderr, "skipped: %s\n", e.what());
        }
    }
});

// Consumer sees only the successful values: 2, 4, 6
for (int n : out) {
    process(n);
}
```

The error channel carries `exception_ptr` values. The receiver rethrows each
one to inspect it -- since it knows what exceptions to expect, it can `catch`
the specific types and extract whatever context they carry.

Key behaviours:

- **Pipeline continues** after each error. Only successful results reach
  downstream.
- **Error channel backpressure** applies: if the error handler is slow,
  `try_map` blocks on the error write, throttling the whole pipeline.
- **Dead error channel** stops `try_map` only when an error actually occurs.
  If you drop the error reader, normal values still flow until the next
  exception -- then `try_map` exits because it cannot deliver the error.
- **Without an error channel**, `try_map(f)` is equivalent to `map(f)` --
  exceptions propagate normally and tear down the pipeline as described above.

## Channel death as control flow

Dropping an endpoint is not an error -- it is the primary mechanism for
signaling completion and cancellation:

- **Writer destroyed** ("no more data"): readers see EOF. Range-for loops
  exit. `operator>>` returns false.
- **Reader destroyed** ("no longer interested"): writers see a dead channel.
  `operator<<` returns false. Producers exit their write loops.

This is normal, expected control flow. A well-structured pipeline tears down
cleanly when any stage drops its endpoints:

```cpp
spawn([w = std::move(ch.w)] {
    for (int i = 0; ; ++i) {
        if (!(w << i)) return;   // reader gone, stop producing
    }
});

// Read 10 values, then drop the reader.
for (int i = 0; i < 10; ++i)
    ch.r.read();
ch.r = {};    // producer sees dead channel and exits
```

## Best practices

**Use `join()` for critical imps.** If an imp's failure should
be visible to the caller, always `join()` on its exception channel. This is
especially important for imps that perform I/O or other operations
where silent failure would cause data loss.

**Use `spawn_range<T>()` for producer-consumer patterns.** The range
automatically propagates exceptions from the producer to the consumer's
iteration loop, making error handling natural and hard to forget.

**Let channel death handle graceful teardown.** Do not throw exceptions to
signal "done" or "cancelled". Instead, drop the relevant endpoint and let the
other side observe the dead channel through its normal read/write loop.

**Set `global_exception_handler` during development.** The default discards
unhandled exceptions silently. Logging them during development helps catch
fire-and-forget imps that fail unexpectedly.
