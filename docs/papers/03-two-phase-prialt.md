# Zero-Overhead Channel Synchronization

## Abstract

Typed channel implementations typically pay for type erasure with an
indirect function call on every data transfer. The channel stores a
function pointer (`tx_`) that knows how to copy or move a value of
the channel's type, and every send or receive invokes it. We describe
a two-phase protocol that eliminates this indirection entirely: the
scheduler finds a matching peer with locks held and returns raw
source/destination pointers; the caller performs the typed transfer
inline at the call site with full compile-time type information. The
result is zero function pointers on the synchronization hot path,
exact static stack analysis, and a RAII wrapper that makes a blocking
channel send look like a single C++ statement.

## 1. The cost of type erasure

A synchronous channel library needs to solve a layering problem. The
channel implementation — locking, wait queues, scheduling — is the
same regardless of the element type. But the data transfer — copying
or moving a `T` from the sender's buffer to the receiver's variable —
depends on `T`. Template instantiation gives each channel type its
own transfer code, but the scheduler that matches senders with
receivers is type-erased: it deals in `void*` pointers and opaque
channel handles.

The traditional solution is a function pointer. Each channel stores a
`tx_` field — a pointer to a function that performs the typed
transfer:

```cpp
// Traditional approach (simplified)
struct Channel {
    std::function<void(void*, void*)> tx_;  // or a raw function pointer
    // ... wait queues, locks ...
};
```

When a sender and receiver rendezvous, the scheduler calls
`ch->tx_(src, dst)` to move the data. This works, but it has two
costs:

**Branch prediction cost.** On ARM64, `tx_` compiles to a `BLR`
(Branch with Link to Register) — an indirect branch. Modern CPUs
predict indirect branches less accurately than direct calls,
especially when different channel types interleave, causing the
branch predictor to thrash.

**Stack analysis cost.** CSP includes an ARM64 instruction walker
that estimates the maximum stack depth of each imp entry
function at spawn time (see [Paper 5](05-stack-engineering.md)).
The walker follows direct calls (`BL`) recursively but cannot follow
`BLR` — it doesn't know the target at analysis time. Every `BLR` in
the call graph forces the analyzer to fall back to a conservative
estimate, potentially over-allocating stack space by orders of
magnitude.

Before the two-phase protocol, the codebase contained 111 spawn entry
functions with at least one `BLR` in their call graph, almost all
from channel transfer function pointers. After the refactoring,
that number dropped to 6 — and those 6 use a dynamic-vector code
path for fan-out operations where the channel count is not known
at compile time.

## 2. The compilation firewall

CSP uses a type-erasure pattern that we call the compilation
firewall. All complex logic — channel locking, wait queue management,
scheduling decisions — lives in `.cc` files behind a narrow C++
interface in `namespace csp::internal`:

```cpp
namespace csp::internal {
    struct WriterRef { void* ptr; };
    struct ReaderRef { void* ptr; };
    struct ChanOp   { Waiter waiter; void* message; };
    struct AltMatch  { int result; void* src; void* dst; char opaque_[128]; };

    void prialt_begin(AltMatch* out, ChanOp const* ops, int count, int nowait);
    void alt_begin(AltMatch* out, ChanOp const* ops, int count, int nowait);
    void alt_end(AltMatch* m);
}
```

The header-side templates (`chan_op<T>`, `writer<T>`, `reader<T>`)
construct `ChanOp` descriptors with `void*` pointers and pass them
to these functions. The internal functions know nothing about `T` —
they deal only in channel pointers, wait queues, and
source/destination addresses.

This keeps the header lightweight (no scheduler implementation, no
lock headers, no imp struct definition) and confines
compilation complexity to a small number of `.cc` files. Users
include one header; the compiler instantiates only the thin template
wrappers.

## 3. The two-phase protocol

The key insight is that the scheduler doesn't need to perform the
data transfer. It only needs to *find* a matching peer and return
the addresses where the data should come from and go to. The
caller — which has full type information — can do the transfer
itself.

### Phase 1: `prialt_begin` (type-erased)

The caller constructs an array of `ChanOp` descriptors and calls
`prialt_begin`. Inside, the function:

1. Locks the relevant channels.
2. Scans the wait queues for a matching peer.
3. If a match is found, extracts the source and destination `void*`
   pointers from the `ChanOp` descriptors.
4. Returns an `AltMatch` with `result` (the index), `src`, and `dst`.
5. Keeps the channel locks held — the transfer has not happened yet.

```cpp
internal::AltMatch m;
internal::prialt_begin(&m, chanops, count, nowait);
// Locks are still held. m.src and m.dst point to the data.
```

### Phase 2: typed transfer (inline)

The caller performs the transfer with full type information:

```cpp
if (m.src && m.dst)
    *static_cast<T*>(m.dst) = std::move(*static_cast<T*>(m.src));
```

This compiles to a direct, inlined move or copy — no function
pointer, no indirection, no `BLR`. The compiler has complete
visibility into `T`'s move constructor and can optimise accordingly.

### Phase 3: `alt_end` (type-erased)

After the transfer, the caller calls `alt_end`, which:

1. Unlocks the channels.
2. Schedules the woken peer for execution.

```cpp
internal::alt_end(&m);
// Locks released. Peer is scheduled.
```

The three phases form a sandwich: type-erased logic on both sides,
with a thin layer of typed code in the middle. The type-erased phases
contain all the complexity (locking, queue management, scheduling).
The typed phase is a single move assignment.

## 4. `chan_op<T>`: RAII synchronization

The two-phase protocol is wrapped in a RAII class, `chan_op<T>`,
that makes channel operations feel like ordinary C++ statements.

A write operation constructs a `chan_op<T>` with the value to send,
stored in an inline aligned-storage buffer:

```cpp
template <typename T>
class chan_op {
    internal::ChanOp chanop_;
    std::aligned_storage_t<sizeof(T), alignof(T)> buf_;
    bool has_buf_ = false;
    mutable bool active_ = true;

public:
    // Write: copy value into inline buffer
    chan_op(internal::WriterRef w, T const& t)
        : chanop_{internal::wait(w), &buf_}, has_buf_(true)
    {
        new (&buf_) T(t);
    }

    // Read: point directly at caller's variable
    chan_op(internal::ReaderRef r, T& dest)
        : chanop_{internal::wait(r), &dest} {}
```

The destructor runs the full three-phase protocol:

```cpp
    ~chan_op() {
        if (active_) {
            internal::AltMatch m;
            internal::prialt_begin(&m, &chanop_, 1, false);
            if (m.src && m.dst)
                *static_cast<T*>(m.dst) = std::move(*static_cast<T*>(m.src));
            internal::alt_end(&m);
        }
        if (has_buf_) reinterpret_cast<T*>(&buf_)->~T();
    }
```

This means that `w << 42;` is a complete blocking send:

1. `operator<<` constructs a `chan_op<int>` (which is a temporary).
2. At the end of the statement, the temporary is destroyed.
3. The destructor calls `prialt_begin` → inline transfer → `alt_end`.
4. The caller's imp blocks until a receiver is ready, the
   data is transferred, and the call returns.

The same `chan_op<T>` can also be used as a boolean — `operator bool`
runs the protocol and returns whether the transfer succeeded:

```cpp
while (w << value) {  // loops until reader dies
    ++value;
}
```

## 5. Variadic alt with compile-time dispatch

When `alt` or `prialt` is called with multiple operations of
different types, the transfer dispatch must select the correct `T` at
runtime (since `prialt_begin` returns an index, not a type). The
library uses a recursive template:

```cpp
template <int I, typename Op, typename... Ops>
void transfer_at(int idx, void* src, void* dst, Op&& op, Ops&&... ops) {
    if (idx == I) {
        std::decay_t<Op>::transfer(src, dst);
    } else {
        transfer_at<I + 1>(idx, src, dst, std::forward<Ops>(ops)...);
    }
}
```

Each `chan_op<T>` provides a static `transfer` function that performs
`*static_cast<T*>(dst) = std::move(*static_cast<T*>(src))`. The
compiler generates a chain of comparisons that resolves to a direct
call at each index — no virtual dispatch, no function pointer table.
With a small number of operations (the common case), the compiler
typically optimises this into a jump table or a series of
branches, all resolved statically.

## 6. The dynamic-vector fallback

Some patterns require a runtime-determined number of channel
operations — fan-out to N workers, for example. For these, the
library provides a `std::vector<internal::ChanOp>` path where the
caller builds the operation list dynamically and passes it to
`prialt_begin` as a flat array.

In this path, the transfer cannot be dispatched at compile time
(the types may vary across vector elements). The library falls back
to a stored function pointer per operation in the vector. This is
the source of the remaining 6 spawn entry functions with `BLR`.

The dynamic path is used only for fan-out/fan-in combinators where
the channel count is unknown at compile time. The vast majority of
channel operations — single sends, single receives, and small
fixed-size alts — use the zero-overhead compile-time path.

## 7. Results

The elimination of `tx_` from the `Channel` struct had three
measurable effects:

**BLR reduction.** Spawn entry functions containing at least one
`BLR` in their transitive call graph dropped from 111 to 6. The
remaining 6 all use the dynamic-vector path.

**Stack analysis precision.** With `BLR` removed from the
template-instantiated paths, the ARM64 instruction walker produces
exact stack depth estimates for these functions. Previously, every
function that touched a channel operation got a conservative
fallback estimate.

**Code generation quality.** The inline `std::move` at the transfer
site gives the compiler full visibility into the move constructor.
For trivially moveable types (`int`, `double`, small structs), the
transfer compiles to a single `str`/`ldr` pair. For types with
non-trivial move constructors, the compiler can inline the
constructor body if it chooses — an option that was unavailable when
the transfer went through a function pointer.

## 8. Related work

Go's channel implementation (`runtime.chansend`, `runtime.chanrecv`)
uses `typedmemmove` — a function that dispatches on the element
size and alignment. This is efficient for Go's value types but is
still an indirect dispatch at the runtime level.

Rust's `crossbeam-channel` uses unsafe pointer casts with
`std::ptr::copy_nonoverlapping`, avoiding function pointers but
requiring careful unsafe code at every transfer site.

CSP's approach is distinctive in that it maintains a clean separation
between the type-erased scheduler (in `.cc` files) and the typed
transfer (in header templates), with no function pointers, no unsafe
casts at the library level, and no compromise on C++ move semantics.
The two-phase protocol makes this separation natural: the scheduler
finds the match; the caller does the transfer; neither needs to know
about the other's internals.
