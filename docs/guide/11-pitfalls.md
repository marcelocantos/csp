# Common Pitfalls

This chapter catalogues mistakes that are easy to make when working with CSP
and explains how to avoid them.

## 1. Forgetting move semantics on endpoints

`writer<T>` and `reader<T>` are move-only. Capturing them by copy in a lambda
is a compile error:

```cpp
// BUG: won't compile -- writer is not copyable
auto [w, r] = csp::chan<int>{};
csp::spawn([w] {           // error: call to deleted copy constructor
    w << 42;
});
```

Move the endpoint into the capture:

```cpp
// FIX: move the endpoint
auto [w, r] = csp::chan<int>{};
csp::spawn([w = std::move(w)] {
    w << 42;
});
```

If you need the endpoint in both the spawning and the spawned microthread, use
`.copy()`:

```cpp
auto [w, r] = csp::chan<int>{};
csp::spawn([w = w.copy()] {
    w << 42;
});
w << 99;   // original w still valid
```

Use `.copy()` deliberately -- it extends the channel's lifetime by incrementing
the reference count. If a `.copy()` is kept alive by accident, the channel
never closes and downstream readers hang forever.

## 2. Microthreads that never exit

If a microthread loops forever, `schedule()` never returns because the runtime
waits for all live microthreads to complete. This is the most common cause of
programs that hang on shutdown.

```cpp
// BUG: loops forever -- schedule() never returns
csp::spawn([r = std::move(r)] {
    for (;;) {
        int n;
        r >> n;
        process(n);
    }
});
```

The loop never checks whether the channel is still alive. Even after all
writers are dropped, the loop continues calling `r >> n`, which returns `false`
but is never tested.

Fix the loop to terminate when the channel closes:

```cpp
// FIX: exit when channel closes
csp::spawn([r = std::move(r)] {
    for (int n : r) {    // range-for exits on channel death
        process(n);
    }
});
```

### The sentinel pattern

Sometimes a microthread reads from a resource (a pipe, a socket) that has no
built-in notion of "channel death". The microthread cannot exit because the
blocking read never returns.

The solution is a *sentinel* -- a helper microthread that watches for channel
death and closes the resource, causing the blocked read to return an error or
EOF. The signal handler demonstrates this pattern:

```cpp
csp::spawn([rfd, wfd, out = std::move(out)] {
    // Sentinel: watch for downstream reader death or producer exit.
    auto out_copy = out.copy();
    auto [kill_w, kill_r] = csp::chan<>{};
    csp::spawn([out_copy = std::move(out_copy),
                kill_r = std::move(kill_r), wfd] {
        // Block until either the downstream reader dies (~out_copy)
        // or the producer exits (~kill_r, because kill_w is destroyed).
        csp::prialt(~out_copy, ~kill_r);
        ::close(wfd);   // unblocks the producer's io::read()
    });

    // Producer loop: reads bytes from the pipe fd.
    uint8_t buf[32];
    for (;;) {
        ssize_t n = csp::io::read(rfd, buf, sizeof(buf));
        if (n <= 0) break;   // EOF -- sentinel closed wfd
        for (ssize_t i = 0; i < n; ++i)
            if (!(out << static_cast<int>(buf[i]))) return;
    }
    ::close(rfd);
    // kill_w destroyed here -> sentinel sees ~kill_r and cleans up
});
```

When the sentinel detects that the downstream reader is gone (`~out_copy`
fires), it closes the write end of the pipe. This causes the producer's
`io::read()` to return 0 (EOF), breaking the loop and allowing the microthread
to exit.

## 3. Deadlock in alt/prialt

Using the same channel on both sides of a single `prialt` is a deadlock. Since
channels are synchronous, a microthread cannot send to itself:

```cpp
// BUG: deadlock -- waiting to read and write the same channel
auto [w, r] = csp::chan<int>{};
int n;
csp::prialt(w << 1, r >> n);   // blocks forever: no other party
```

A more subtle variant is two microthreads each trying to write to each other
with no reader:

```cpp
// BUG: mutual deadlock
auto [w1, r1] = csp::chan<int>{};
auto [w2, r2] = csp::chan<int>{};

csp::spawn([w = std::move(w1), r = std::move(r2)] {
    w << 1;   // blocks: nobody reading r1
    int n;
    r >> n;
});
csp::spawn([w = std::move(w2), r = std::move(r1)] {
    w << 2;   // blocks: nobody reading r2
    int n;
    r >> n;
});
```

Both microthreads block on their sends because neither reaches its receive.
Fix this by ensuring at least one side reads first, or by using `alt` to
attempt both operations simultaneously:

```cpp
// FIX: use alt to do both at once
csp::spawn([w = std::move(w1), r = std::move(r2)] {
    int n;
    csp::alt(w << 1, r >> n);   // whichever is ready first
    // ... handle remaining operation ...
});
```

## 4. Forgetting init_runtime() for I/O

I/O operations (`io::wait_readable`, `io::wait_writable`), the blocking pool
(`csp::blocking()`), and signal handling (`csp::signal::notify`) all require
the M:N runtime. Without `init_runtime()`, the event reactor is not started
and these calls will hang or crash:

```cpp
// BUG: io::read() hangs -- no reactor running
csp::spawn([fd] {
    char buf[1024];
    csp::io::read(fd, buf, sizeof(buf));   // hangs
});
csp::schedule();
```

Call `init_runtime()` before any I/O:

```cpp
// FIX: enable M:N runtime
csp::init_runtime(2);   // at least 2 processors for I/O

csp::spawn([fd] {
    char buf[1024];
    csp::io::read(fd, buf, sizeof(buf));   // works
});

csp::schedule();
csp::shutdown_runtime();
```

## 5. Capturing stack references

A spawned microthread runs concurrently. If it captures a local variable by
reference, the variable may be destroyed before the microthread reads it:

```cpp
// BUG: dangling reference
void start_worker() {
    int config = 42;
    csp::spawn([&config] {      // captures by reference
        use(config);            // undefined behavior: config is gone
    });
    // config destroyed here, but the microthread is still running
}
```

Capture by value instead:

```cpp
// FIX: capture by value
void start_worker() {
    int config = 42;
    csp::spawn([config] {       // copies config into the lambda
        use(config);            // safe
    });
}
```

For large objects, use `std::shared_ptr`:

```cpp
// FIX: shared ownership for large objects
void start_worker() {
    auto config = std::make_shared<Config>(load_config());
    csp::spawn([config] {
        use(*config);
    });
}
```

## 6. Blocking the processor thread

Calling blocking system calls directly from a microthread stalls the OS thread,
preventing all other microthreads on that processor from running:

```cpp
// BUG: blocks the entire processor
csp::spawn([] {
    auto result = getaddrinfo(...);   // blocks OS thread
    // all other microthreads on this processor are frozen
});
```

Use `csp::blocking()` to offload the call to a thread pool:

```cpp
// FIX: run on the blocking pool
csp::spawn([] {
    auto result = csp::blocking([] {
        return getaddrinfo(...);
    });
    // processor was free to run other microthreads while DNS resolved
});
```

For file descriptor I/O, use the non-blocking wrappers in `csp::io` instead of
raw system calls:

```cpp
// BUG: raw read blocks the processor
::read(fd, buf, len);

// FIX: csp::io::read suspends cooperatively
csp::io::read(fd, buf, len);
```

The `csp::io` wrappers set the fd to non-blocking mode, suspend the
microthread on EAGAIN, and retry when the fd becomes ready -- all without
stalling the processor.

## 7. Channel death propagation in pipelines

In a multi-stage pipeline, if one stage crashes or exits early, the effects
propagate in both directions:

- **Upstream**: writers see `false` from `w << val` (broken pipe).
- **Downstream**: readers see EOF (range-for exits, `r >> n` returns `false`).

If stages do not handle these cases, the pipeline hangs:

```cpp
// BUG: stage B crashes, stage A loops forever
auto [ab_w, ab_r] = csp::chan<int>{};
auto [bc_w, bc_r] = csp::chan<int>{};

csp::spawn([w = std::move(ab_w)] {
    for (int i = 0; ; ++i)
        w << i;        // never checks return value
});

csp::spawn([r = std::move(ab_r), w = std::move(bc_w)] {
    int n = r.read();
    throw std::runtime_error("oops");   // stage B dies
    // w destroyed -> stage C sees EOF
    // r destroyed -> stage A's send should fail, but A ignores it
});
```

Always check the return value of sends, or use range-for loops that
automatically terminate on channel death:

```cpp
// FIX: handle channel death at every stage
csp::spawn([w = std::move(ab_w)] {
    for (int i = 0; w << i; ++i) {}   // exits when reader dies
});

csp::spawn([r = std::move(ab_r), w = std::move(bc_w)] {
    for (int n : r) {                  // exits when writer dies
        if (!(w << transform(n)))      // exits when reader dies
            break;
    }
});
```

## 8. SIGPIPE with pipes

On Unix, writing to a pipe or socket whose read end is closed delivers SIGPIPE,
which terminates the process by default. This can happen when a downstream
microthread drops its reader before the writer finishes:

```cpp
// BUG: SIGPIPE kills the process
int pipefd[2];
::pipe(pipefd);
csp::spawn([fd = pipefd[1]] {
    const char* msg = "hello";
    ::write(fd, msg, 5);     // SIGPIPE if read end is already closed
});
::close(pipefd[0]);           // close read end
```

On macOS, suppress SIGPIPE per-fd with `F_SETNOSIGPIPE`:

```cpp
// FIX (macOS): suppress SIGPIPE on this fd
#ifdef F_SETNOSIGPIPE
fcntl(pipefd[1], F_SETNOSIGPIPE, 1);
#endif
```

Alternatively, ignore SIGPIPE globally (this is safe -- well-written code
checks write() return values):

```cpp
// FIX (portable): ignore SIGPIPE process-wide
signal(SIGPIPE, SIG_IGN);
```

After suppressing the signal, `write()` returns -1 with `errno == EPIPE`
instead of killing the process. Check the return value and handle the error.

## 9. Double-close of file descriptors

When multiple microthreads share a file descriptor, closing it from one
microthread while another is still using it causes undefined behavior
(the fd number may be reused by the OS for a new file):

```cpp
// BUG: double close / use-after-close
int fd = open_connection();
csp::spawn([fd] {
    csp::io::read(fd, buf, len);
    ::close(fd);               // first close
});
csp::spawn([fd] {
    csp::io::write(fd, data, len);
    ::close(fd);               // second close -- undefined behavior
});
```

Use an RAII wrapper to ensure the fd is closed exactly once, and coordinate
ownership through channels:

```cpp
// FIX: RAII wrapper, single owner
struct OwnedFd {
    int fd;
    OwnedFd(int fd) : fd(fd) {}
    ~OwnedFd() { if (fd >= 0) ::close(fd); }
    OwnedFd(OwnedFd&& o) : fd(o.fd) { o.fd = -1; }
    OwnedFd& operator=(OwnedFd&&) = delete;
    OwnedFd(const OwnedFd&) = delete;
};

// Single microthread owns the fd
csp::spawn([fd = std::make_shared<OwnedFd>(open_connection()),
            done_r = std::move(done_r)] {
    char buf[1024];
    csp::io::read(fd->fd, buf, sizeof(buf));
    // fd closed automatically when shared_ptr refcount hits 0
});
```

A simpler approach is to designate a single microthread as the fd owner and
have other microthreads communicate with it through channels rather than
sharing the fd directly.
