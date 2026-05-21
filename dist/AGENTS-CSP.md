# CSP Agent Guide

Token-efficient reference for coding agents. Covers the full API surface,
common idioms, and critical gotchas. For narrative explanations see `guide/`.

## Files

CSP is distributed as three files:

| File | Content |
|---|---|
| `csp.h` | Single header: core API, timers, I/O, signals, blocking, cancellation, dynamic scoping, 70+ stream combinators |
| `csp.cpp` | Implementation source + fcontext inline assembly |
| `csp_globals.cpp` | Thread-local state (must be a separate TU) |

All user code needs only `#include "csp.h"`.

## Core Types

```cpp
namespace csp {

// Synchronous typed channel. Default T = poke_t (empty message).
template <typename T = poke_t> struct chan { writer<T> w; reader<T> r; };

// Move-only endpoints. Use .copy() for shared ownership.
template <typename T = poke_t> class writer;  // w << val  → chan_op<T>
template <typename T = poke_t> class reader;  // r >> var  → chan_op<T>

// RAII channel operation. Destructor blocks (calls prialt).
// operator bool() blocks and returns true if data transferred, false if dead.
template <typename T> class chan_op;

// Empty-message sentinel. chan<> is shorthand for chan<poke_t>.
extern struct poke_t {} poke;

// Dead-channel reader (always matches immediately as dead).
extern reader<> const skip;

// Non-blocking guard. Fires when no other op is ready.
// Returns csp::none (INT_MIN), usable as a switch case label.
//   switch (prialt(ch >> val, csp::none)) {
//     case 0:         /* read matched  */ break;
//     case csp::none: /* nothing ready */ break;
//   }
inline constexpr none_t none{};
}
```

## Lifecycle

```cpp
// Spawn an imp. Returns reader<exception_ptr> (join handle).
// f is taken BY VALUE (moved into the imp).
template <typename F> reader<std::exception_ptr> spawn(F&& f);

// Block until spawned imp finishes; rethrows its exception.
void join(reader<std::exception_ptr> const& r);

// Run the scheduler (blocks until all imps complete).
void schedule();

// M:N runtime (auto-initializes on first use with hardware concurrency).
// Override: set_maxprocs(n), or CSP_MAXPROCS env var. 1 = single-threaded.
void set_maxprocs(int num_procs = 0);  // 0 = hardware_concurrency
void shutdown_runtime();

// Cooperative yield (no-op outside an imp).
void yield();

// Imp exit interception: supervised + on_exit.
struct restart_policy { int max_restarts=3; duration window=5s; duration backoff=0s; };

// Custom handler: inspect event, call ev.restart() or drop.
auto guard = on_exit([](imp_event ev) {
    if (ev.error) ev.restart(1s);  // restart on exception
});
spawn(supervised([]() { do_work(); }));

// Policy-based: automatic restart with sliding window.
auto guard = on_exit(restart_policy{.max_restarts = 5});
spawn(supervised([]() { serve(); }));

// Only spawn(supervised(f)) is intercepted; regular spawn is unaffected.
// Dynamic scoping: child imps inherit the exit handler automatically.
```

## Channel Operations

```cpp
// Create a channel.
auto [w, r] = chan<int>{};       // synchronous (rendezvous)
auto [bw, br] = chan<int>(16);   // buffered (capacity 16, spawns buffer imp)

// Shorthand: create channel from endpoint reference.
reader<int> r = --w;   // w must be unattached
writer<int> w = ++r;   // r must be unattached

// Write (blocks until reader accepts or channel dies).
w << 42;                     // statement: blocks via chan_op destructor
if (w << 42) { /* sent */ }  // expression: blocks, tests success

// Read (blocks until writer sends or channel dies).
int v;
r >> v;                      // statement: blocks
if (r >> v) { /* got v */ }  // expression: blocks, tests success

// Read and return (throws csp::error if dead).
int v = r.read();

// Range-for over reader (reads until channel dies).
for (int v : r) { process(v); }

// Pipe reader directly to writer (blocks until either dies).
spawn(r.stream_to(std::move(w)));

// Shared ownership (endpoints are move-only by default).
auto w2 = w.copy();   // increments refcount
auto r2 = r.copy();

// Death watch (fires when the other endpoint is dropped).
chan_op<T> op = ~w;    // fires when all readers of w's channel die
chan_op<T> op = ~r;    // fires when all writers of r's channel die

// Topology: swap, fuse, tap.
swap(a.w, b.w);         // redirect a's writers to b's channel and vice versa
swap(a.r, b.r);         // same for readers
fuse(a.w, b.r);         // redirect a.w and b.r onto a shared temp channel
                         // a.r sees writer death; b.w sees reader death

// 4-arg swap (fuse/split modes):
swap(a.w, {}, {}, b.r);                             // fuse mode
swap(w, std::move(a.r), std::move(b.w), r);          // split mode

// Tap: observe values on a channel without modifying the pipeline.
auto tr = tap(ch.w, ch.r);   // tr is reader<T> seeing every value
// Both tr and ch.r must be consumed for the pipeline to flow.
tr = {};                      // destroying the reader auto-fuses w,r back

// Splice: insert a custom filter between w and r; auto-fuses back on return.
splice(ch.w, ch.r, [](reader<int> in, writer<int> out) {
    for (int v; in >> v;) out << v * 2;   // transform, filter, rate-limit, etc.
});

// Pipe operator: syntactic sugar for fuse(w, r).
w | r;   // equivalent to fuse(w, r)

// In-band exception delivery: send an exception in place of a value.
// The reader rethrows at its r >> v / prialt(...) call site; the channel
// stays live and continues to carry values or further exceptions.
w._throw(std::make_exception_ptr(my_error("boom")));
try { r >> v; } catch (my_error const& e) { /* observed */ }
// Buffered channels carry exceptions in-order with values.
// `writer<exception_ptr>` disambiguates: `<<` sends as value (no throw at
// the reader), `_throw` sends as exception (rethrown at the reader).
```

## Request / Response

```cpp
// Typed request with embedded reply channel.
template <typename Req, typename Resp> struct request {
    using value_type = Req;
    using reply_type = Resp;
    Req value;
    writer<Resp> reply;
};

// Trait.
template <typename T> struct is_request : std::false_type {};
// is_request<request<Req, Resp>>::value == true

// Non-blocking call: sends request, returns reader for response.
template <typename Req, typename Resp>
reader<Resp> call(writer<request<Req, Resp>>& w, Req req);

// Blocking call: writer::operator()(Req) → Resp (requires is_request<T>).
// Equivalent to call(*this, req).read().
auto resp = w(req);
```

Example:
```cpp
auto [w, r] = chan<request<std::string, int>>{};

// Server loop.
spawn([r = std::move(r)] {
    request<std::string, int> msg;
    while (r >> msg) msg.reply << (int)msg.value.size();
});

// Client — blocking.
int len = w(std::string("hello"));  // 5

// Client — non-blocking / concurrent.
auto r1 = call(w, std::string("abc"));
auto r2 = call(w, std::string("defgh"));
int a = r1.read();  // 3
int b = r2.read();  // 5
```

## Alt / Prialt

Select over multiple channel operations. `prialt` tries in order; `alt`
randomizes. Both block until one operation completes.

```cpp
// Return value:
//   non-negative n → operation n matched (0-based)
//   complement ~n  → death event for operation n (0-based)

int v;
switch (prialt(w << 42, r >> v, ~some_reader)) {
    case 0:  /* wrote 42 */          break;
    case 1:  /* read into v */       break;
    case ~0: /* w's reader died */   break;
    case ~1: /* r's writer died */   break;
    case ~2: /* ~some_reader: writer of some_reader died */ break;
}
```

Key: `~endpoint` is a death-watch operation. When it fires, the return
value is **complemented** (`~k` for the `k`-th operation). All death
events — both explicit vultures and implicit death on data operations —
return complemented indices. This **bidirectional lifecycle observability**
— each endpoint's death independently observable by the other side — is
CSP's core design principle. See `docs/papers/15-channels-as-interfaces.md`.

**`after()` returns non-negative**: `after(d)` sends a `time_point` then
dies, so `prialt(ch >> v, after(1s) >> nullptr)` returns `1` on timeout (data
match on the time_point value), not `~1`.

```cpp
// Vector overload (all ops must be same type T).
std::vector<chan_op<int>> ops;
ops.push_back(r1 >> v);
ops.push_back(r2 >> v);
int result = alt(ops);  // or prialt(ops)

// Non-blocking poll with none.
switch (prialt(r >> v, csp::none)) {
    case 0:         /* got v */       break;
    case csp::none: /* would block */ break;
}

// Vector overload with none.
int result = alt(ops, csp::none);  // or prialt(ops, csp::none)
```

**Vultures as control signals:** Destroying an endpoint deliberately fires
`~ep` in any imp watching it, giving you a composable interrupt. The signaller
holds the endpoint copy; the watcher uses the vulture. **Never** store a copy
of the watched endpoint inside the watching imp -- the copy keeps it alive,
creating a reference cycle that prevents the death event from ever firing.

**`closer<EP>` -- vulture-only wrapper.** Restricts an endpoint to
death-watch + liveness-check. No `>>`, no `<<` -- only `~handle` and
`bool(handle)`. Useful for spawn handles where the channel value is
consumed elsewhere (e.g. `reader<exception_ptr>` from `spawn`).

```cpp
csp::closer handle(csp::spawn([] { do_work(); }));  // CTAD
if (handle) { /* still running */ }
auto k = csp::prialt(~handle);                       // matches on imp exit
handle.endpoint();                                   // escape hatch
```

## Spawn Helpers

```cpp
// Spawn a producer imp, return its output reader.
template <typename T, typename F>
reader<T> spawn_producer(F&& f);
// f signature: void(writer<T>)

// Spawn a consumer imp, return its input writer.
template <typename T, typename F>
writer<T> spawn_consumer(F&& f);
// f signature: void(reader<T>)

// Spawn a filter imp, return {input_writer, output_reader}.
template <typename T, typename F>
chan<T> spawn_filter(F&& f);
// f signature: void(reader<T>, writer<T>)

// Spawn a producer with exception propagation via range iterator.
template <typename T, typename F>
range<T> spawn_range(F&& f);
// for (auto& v : spawn_range<int>(f)) { ... }  // rethrows on iteration
```

## Timers

```cpp
using time_point = std::chrono::steady_clock::time_point;
using duration = std::chrono::steady_clock::duration;

void sleep(duration d);
void sleep_until(time_point tp);

// One-shot: reader<time_point> that fires after duration, then dies.
reader<time_point> after(duration d);

// Periodic: reader<time_point> that fires at interval (drift-free).
reader<time_point> tick(duration interval);
```

Timeout idiom:
```cpp
int v;
switch (prialt(r >> v, after(100ms) >> nullptr)) {
    case 0:  handle(v);  break;
    case 1:  timeout();  break;   // after() sent time_point → non-negative match
}
```

### Fake Clock (testing)

```cpp
class fake_clock;
extern dynamic<clock_source*> clock;   // real_clock by default

time_point now();   // Returns fake time if overridden, real otherwise.

class fake_clock {
public:
    explicit fake_clock(time_point start = time_point{});
    time_point now() const;
    bool has_pending() const;
    void advance(duration d);              // Advance time, fire expired timers.
    bool advance_to_next();                // Jump to next deadline (false if empty).
    void run();                            // Scheduler loop with auto-advance.
    void run_until_idle();                 // Run ready imps, don't advance time.
};
```

Bind via dynamic scoping — inherited by child imps automatically:
```cpp
fake_clock fc;
csp::local l{csp::clock = &fc};
// All sleep/after/tick calls now use fc.
```

Single-threaded only. `sleep`, `after`, `tick` all respect the override.

## I/O

Uses kqueue reactor (macOS), epoll (Linux), IOCP (Windows).

```cpp
namespace csp::io {

// Opaque fd wrapper. No implicit int conversion.
struct fd_t {
    explicit fd_t(int raw_fd = -1);
    int raw() const;          // access the underlying descriptor
    bool valid() const;       // raw() >= 0
    bool is_nonblock() const; // O_NONBLOCK is set
    explicit operator bool() const;  // same as valid()
    // Comparison operators (==, !=, <, etc.)
    // Move-only: copy is deleted.
};

// Layer 1: suspend until fd ready.
void wait_readable(fd_t fd);
void wait_writable(fd_t fd);

// Layer 2: auto-retry on EAGAIN/EINTR.
ssize_t read(fd_t fd, void* buf, size_t len);   // 0=EOF, -1=error
ssize_t write(fd_t fd, const void* buf, size_t len); // writes ALL, -1=error
fd_t accept(fd_t listen_fd, sockaddr* addr, socklen_t* addrlen); // returned fd is non-blocking
int connect(fd_t fd, const sockaddr* addr, socklen_t addrlen); // 0=ok

// Convenience: read until EOF.
std::vector<uint8_t> read_all(fd_t fd);

// Convenience: write all bytes, throw on failure.
void write_all(fd_t fd, std::span<const uint8_t> data);

// Utility.
int set_nonblock(fd_t fd);

// DNS (runs on blocking pool).
// resolve_result { addrinfo_ptr info; int error; explicit operator bool(); const char* message(); }
resolve_result resolve(const std::string& host,
                       const std::string& service = {},
                       const addrinfo* hints = nullptr);
}
```

```cpp
namespace csp::file {
// Read entire file into a vector (runs on blocking pool).
std::vector<uint8_t> read(const std::string& path);

// Write data to file (runs on blocking pool). Throws on failure.
void write(const std::string& path, std::span<const uint8_t> data);
}
```

Layer 3 parts:
```cpp
// byte_reader: fd → reader<vector<uint8_t>>. Owns fd (must be non-blocking), closes on exit.
auto r = csp::part::io::byte_reader(fd).spawn();          // 4096 chunks
auto r = csp::part::io::byte_reader(fd, 65536).spawn();   // custom size

// byte_writer: writer<vector<uint8_t>> → fd. Owns fd (must be non-blocking), closes on exit.
auto w = csp::part::io::byte_writer(fd).spawn();

// lines: fd → reader<string> (composes byte_reader | split_lines).
auto lr = csp::part::io::lines(fd).spawn();

// split_lines: byte stream → string stream (LF-delimited).
auto lr = csp::part::io::split_lines.spawn(std::move(byte_reader));

// fixed_frames: byte stream → fixed-size frames.
auto fr = csp::part::io::fixed_frames(512).spawn(std::move(byte_reader));

// Pipeline composition:
auto lr = csp::part::io::byte_reader(fd) | csp::part::io::split_lines;
auto line_reader = lr.spawn();
```

## Networking

```cpp
namespace csp::net {

struct connection {
    io::fd_t fd;
    reader<bytes> input;    // bytes from peer
    writer<bytes> output;   // bytes to peer
    std::string remote_addr;
};

struct listener {
    reader<connection> connections;  // read to accept
    uint16_t port;                  // actual bound port
    std::string local_addr;
};

listener listen(uint16_t port, listen_options opts = {});
connection dial(const std::string& host, uint16_t port);
}
```

## HTTP

Channel-native HTTP/1.1 server. Drop-in: `csp_http.cpp`; link against llhttp.

```cpp
namespace csp::http {

enum class method { GET, HEAD, POST, PUT, DELETE_, PATCH, OPTIONS, CONNECT, TRACE };
const char* method_name(method m);

struct response {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;
};

struct request {
    http::method method;
    std::string url;
    uint8_t version_major, version_minor;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;                    // buffered request body
    bool keep_alive;
    writer<response> respond;      // write exactly one response

    // WebSocket / protocol upgrade: after writing a 101 response via
    // respond, read the raw fd from this channel (ws::upgrade does this).
    struct hijack_result { io::fd_t fd; bytes leftover; };
    reader<hijack_result> hijack;

    std::string header(const std::string& name) const;  // case-insensitive
    int64_t content_length() const;                       // -1 if absent
};

struct endpoint {
    reader<request> requests;      // one per HTTP request
    std::string remote_addr;
};

struct server {
    reader<endpoint> endpoints;    // one per connection
    uint16_t port;
    std::string local_addr;
};

server serve(uint16_t port, serve_options opts = {});

// --- Client ---

response fetch(method m, const std::string& url,
    std::vector<std::pair<std::string, std::string>> headers = {},
    bytes body = {}, fetch_options opts = {});

// Convenience wrappers:
response get(const std::string& url, headers = {}, opts = {});
response post(const std::string& url, bytes body, headers = {}, opts = {});
}
```

Server usage:
```cpp
auto srv = http::serve(8080);
http::endpoint ep;
while (srv.endpoints >> ep) {
    csp::spawn([ep = std::move(ep)] {
        http::request req;
        while (ep.requests >> req) {
            std::string body = "Hello!";
            req.respond << http::response{200,
                {{"Content-Type", "text/plain"}},
                bytes(body.begin(), body.end())};
        }
    });
}
```

Client usage:
```cpp
// Simple GET
auto resp = http::get("http://example.com/api/data");
// resp.status, resp.headers, resp.body

// POST with body and custom headers
std::string payload = R"({"key":"value"})";
auto resp = http::post("http://example.com/api",
    bytes(payload.begin(), payload.end()),
    {{"Content-Type", "application/json"}});
```

## WebSocket

Channel-native WebSocket (RFC 6455). Drop-in: `csp_ws.cpp`; link against wslay
(plus `csp_http.cpp` + llhttp for the upgrade flow).

```cpp
namespace csp::ws {

enum class opcode : uint8_t { text=0x1, binary=0x2, close=0x8, ping=0x9, pong=0xA };

struct message {
    opcode op   = opcode::binary;
    bytes  data;
};

struct conn {
    reader<message> recv;   // inbound text/binary messages
    writer<message> send;   // outbound messages; drop to initiate Close
};

// Server-side: call inside an HTTP handler when Upgrade: websocket.
// Validates headers, sends 101, hijacks fd. Throws csp::error on bad headers.
conn upgrade(http::request& req);

// Client-side: ws://host[:port]/path only (no wss://).
// Throws csp::error on failure.
conn connect(const std::string& url);

} // namespace csp::ws
```

WebSocket upgrade (server):
```cpp
auto srv = http::serve(8080);
http::endpoint ep;
while (srv.endpoints >> ep) {
    csp::spawn([ep = std::move(ep)] {
        http::request req;
        while (ep.requests >> req) {
            if (req.header("Upgrade") == "websocket") {
                auto wsc = ws::upgrade(req);
                ws::message msg;
                while (wsc.recv >> msg) { wsc.send << std::move(msg); }
            } else {
                req.respond << http::response{200, {}, {}};
            }
        }
    });
}
```

WebSocket client:
```cpp
auto wsc = ws::connect("ws://example.com/chat");
ws::message out;
out.op   = ws::opcode::text;
out.data = bytes{/*...*/};
wsc.send << std::move(out);
ws::message in;
if (wsc.recv >> in) { /* process reply */ }
// Drop wsc.send to initiate close handshake.
```

Close handshake: drop `conn.send` → writer sends Close frame → peer echoes →
`conn.recv` closes. Ping/pong handled automatically (not visible to user).

## Signals

```cpp
// Unix: returns reader<int> emitting signal numbers.
auto sig = csp::signal::notify({SIGINT, SIGTERM});
int s;
switch (prialt(data >> v, sig >> s)) {
    case 0: process(v);  break;
    case 1: shutdown(s);  break;
}

// Windows: returns reader<DWORD> emitting console control events.
auto sig = csp::win::signal::notify({CTRL_C_EVENT, CTRL_CLOSE_EVENT});
DWORD ev;
switch (prialt(data >> v, sig >> ev)) {
    case 0: process(v);   break;
    case 1: shutdown(ev);  break;
}
```

## Blocking

```cpp
// Run fn on OS thread pool; suspend calling imp until done.
template <typename Fn> auto blocking(Fn&& fn) -> invoke_result_t<Fn>;

// Example:
auto result = csp::blocking([]{ return expensive_syscall(); });
```

## Dynamic Scoping

```cpp
// Typed dynamic variable. *var reads, var = val creates a binding.
csp::dynamic<int> depth(0);

// local: RAII scoped binding (reverts when l is destroyed).
{ csp::local l{depth = *depth + 1};
  // *depth == 1 here
}  // depth restored to 0

// Multiple bindings in one local.
csp::local l{depth = 1, user = std::string("alice")};

// Bare assignment asserts in debug (catches accidental unscoped mutations).
depth = 42;  // assert failure + [[nodiscard]] warning

// context: snapshot + transfer across channels.
auto ctx = csp::context::current();
spawn([ctx] {
    csp::context_scope scope(ctx);  // install foreign context
    // *depth == 1 here
});

// Spawned imps inherit parent's context automatically.

// Imp-local (not inherited, direct write).
csp::imp_local<int> counter;
counter = 42;       // direct write, no local needed
int v = *counter;   // 42
// Child imps start with default (0), not parent's value.
```

**Outside any imp** (e.g. directly from `main()` before `csp::run` /
`csp::spawn`, or from a foreign thread not bound to the runtime):
- `*var` and `var->...` on `dynamic<T>` / `imp_local<T>` -- return the
  default value (no binding can exist on a non-existent imp).
- `csp::local{...}`, `imp_local::operator=`, `context_scope` -- throw
  `csp::error` with a message naming the API and pointing to
  `csp::run` / `csp::spawn`.
- `context::current()` -- returns an empty context.

## Cancellation

Cooperative scope-based cancellation via dynamic scoping.

```cpp
#include "csp.h"    // includes cancel.h

// Exceptions.
struct canceled : csp::error {};    // base cancel exception
struct timed_out : canceled {};     // deadline cancel exception

// Create a cancel scope. Auto-cancels when guard is destroyed.
cancel_guard cancellation();
cancel_guard cancellation(duration d);     // deadline
cancel_guard cancellation(time_point tp);  // deadline

// Manual cancel: guard() or guard(exception_ptr).
auto guard = cancellation(5s);
guard();                               // cancel with canceled{}
guard(std::make_exception_ptr(my_error{}));  // cancel with custom reason

// Cancel-aware prialt: done() returns a chan_op<> vulture.
int v;
auto cop = done();
switch (prialt(std::move(cop), r >> v)) {
    case ~0: /* cancelled */ break;
    case 1:  /* got v */     break;
}

// Query cancel state.
bool active = is_cancel_active();
std::exception_ptr reason = cancel_reason();
```

Sleep, I/O, and `after()` are cancel-aware — they throw `canceled` or
`timed_out` when a cancel scope fires. Child imps inherit the cancel
scope automatically via dynamic scoping.

## TLS

Cancel-aware TLS 1.3 via PicoTLS (minicrypto backend). Available when
`CSP_TLS` is defined (default in dev builds).

```cpp
#include "csp.h"    // includes tls.h (behind #ifdef CSP_TLS)

namespace csp::tls {

struct error : csp::error { int code; };  // wraps PicoTLS error

using verify_fn = std::function<bool(const char* server_name,
    const std::vector<std::vector<uint8_t>>& certs)>;

class context {
    enum role { client, server };
    explicit context(role r = client);
    void load_cert(const char* cert_pem_path);   // PEM file
    void load_key(const char* key_pem_path);     // PKCS#8 PEM, secp256r1
    void set_verify(verify_fn fn);               // custom cert verification
};

class conn {
    conn(context& ctx, io::fd_t fd);  // fd must be non-blocking, connected
    void set_hostname(const std::string& h);  // SNI
    void handshake();              // cancel-aware
    ssize_t read(void* buf, size_t len);   // 0 = clean shutdown
    ssize_t write(const void* buf, size_t len);  // writes all
    void shutdown();               // close_notify
    io::fd_t fd() const;
};

}
```

`conn` does not own the fd. All blocking methods are cancel-aware via
`wait_readable`/`wait_writable`. No built-in X.509 chain verification —
use `set_verify()` for custom validation. No stream parts — TLS conn is
not safe for concurrent read+write.

Dist users: `#define CSP_TLS` before `#include "csp.h"`, link own PicoTLS.

## QUIC

QUIC transport over UDP via ngtcp2 + PicoTLS (minicrypto). Available when
`CSP_TLS` is defined. Drop-in: `csp_quic.cpp` plus
`ngtcp2_crypto_picotls_minicrypto.c` (the C99 adapter, compiled with the C
compiler); link against ngtcp2 + PicoTLS. Calling any `csp::quic::` symbol
pulls `csp_tls.cpp` in automatically.

```cpp
#include "csp.h"    // includes quic.h (behind #ifdef CSP_TLS)

namespace csp::quic {

struct stream_pair {
    reader<std::vector<uint8_t>> input;   // bytes from peer (EOF = FIN)
    writer<std::vector<uint8_t>> output;  // bytes to peer  (drop = FIN)
};

struct connection {
    stream_pair open_stream();           // blocks until stream established
    reader<stream_pair> incoming_streams; // accept peer-initiated streams
    std::string remote_addr;
};

struct listen_options {
    uint64_t max_streams_bidi = 128;
    int rcvbuf = 0;
    std::string cert_pem;  // path to PEM cert file
    std::string key_pem;   // path to PEM private key file
};

struct listener {
    reader<connection> connections;  // one per accepted QUIC connection
    uint16_t port;
    std::string local_addr;
};

listener listen(uint16_t port, listen_options opts = {});

struct dial_options {
    uint64_t max_streams_bidi = 128;
};

connection dial(const std::string& host, uint16_t port, dial_options opts = {});

}
```

**Server pattern:**
```cpp
csp::quic::listen_options lo;
lo.cert_pem = "server.crt";
lo.key_pem  = "server.key";
auto lst = csp::quic::listen(0, lo);  // port=0 → OS assigns
uint16_t port = lst.port;

csp::quic::connection conn;
lst.connections >> conn;

csp::quic::stream_pair sp;
conn.incoming_streams >> sp;

std::vector<uint8_t> data;
while (sp.input >> data) {
    sp.output << data;  // echo
}
```

**Client pattern:**
```cpp
auto conn = csp::quic::dial("127.0.0.1", port);
auto sp   = conn.open_stream();

sp.output << std::vector<uint8_t>{'h','i'};
sp.output = {};  // send FIN

std::vector<uint8_t> reply;
sp.input >> reply;
```

Each accepted QUIC connection is driven by the listener imp (server) or a
dedicated io imp (client). Streams are unbuffered CSP channels — the driving
loop blocks per-chunk until the app reads, so keep stream consumers responsive.

## Parts System

Three wrapper types for composable stream stages:

| Type | Wraps | `spawn()` returns | Bound endpoint |
|---|---|---|---|
| `producer<T,F>` | `void(writer<T>)` | `reader<T>` | output |
| `consumer<T,F>` | `void(reader<T>)` | `writer<T>` | input |
| `filter<In,Out,F>` | `void(reader<In>, writer<Out>)` | `spawn(reader<In>)→reader<Out>`, `spawn(writer<Out>)→writer<In>` | either |

Factories: `make_producer<T>(f)`, `make_consumer<T>(f)`, `make_filter<In,Out>(f)`.

### Composition with `|`

```cpp
// filter | filter → filter     producer | filter → producer
// filter | consumer → consumer producer | consumer → callable
// reader | filter → reader     filter | writer → writer
// reader | consumer → callable producer | writer → callable

auto pipeline = csp::part::map<int>([](int x){ return x*2; })
              | csp::part::where<int>([](int x){ return x > 5; });
auto r = pipeline.spawn(std::move(source_reader));
```

### Canonical combinator loop

Most filters follow this pattern:
```cpp
auto f = csp::part::make_filter<In, Out>([](reader<In> r, writer<Out> w) {
    for (In v; r >> v;) {
        if (!(w << transform(v))) break;
    }
});
```

## Combinator Reference

All in `namespace csp::part` (included via `csp.h`).

| Combinator | Kind | Description |
|---|---|---|
| `all_of<T>(pred)` | filter | Short-circuiting universal quantifier; emits single bool |
| `any_of<T>(pred)` | filter | Short-circuiting existential quantifier; emits single bool |
| `batch<T>(n)` | filter | Collect n elements into `vector<T>` |
| `bernoulli(p)` | producer | Random bools with configurable probability |
| `blackhole<T>()` | consumer | Discard all values |
| `chain<T>(readers...)` | producer | Concatenate readers sequentially |
| `choice(container)` | producer | Random picks from a container |
| `chunk_by<T>(f)` | filter | Group consecutive elements where `f(prev,curr)` is true |
| `collect<T>(iter)` | consumer | Consume stream into output iterator |
| `concat_all<T>` | filter | Flatten `reader<reader<T>>` sequentially |
| `combine_latest<Ts...>(readers...)` | producer | Emit tuple of latest values whenever any input updates |
| `conflate<T>(f)` | filter | Merge pending values when downstream is slow |
| `count<T>(start,stop,step)` | producer | Numeric sequence [start,stop) |
| `count_forever<T>(start,step)` | producer | Unbounded numeric sequence |
| `deaf<T>()` | consumer | Never-accepting endpoint |
| `debounce<T>(dur,cfg)` | filter | Emit after quiet period, suppress rapid fire |
| `default_if_empty<T>(val)` | filter | Emit default if input closes empty |
| `delay<T>(dur)` | filter | Delay each value independently |
| `distinct<T>()` | filter | Suppress consecutive duplicates |
| `enumerate<T>(container)` | producer | Stream container elements |
| `exhaust_all<T>` | filter | Flatten sub-streams, ignoring new while active |
| `fallback<T>(readers)` | producer | Sequential failover: try each reader, use first that produces |
| `fanout<T>(n)` | filter | Broadcast to dynamic subscriber set |
| `first<T>(n)` | filter | Take first n elements |
| `first_wins<T>(readers...)` | producer | Read from whichever source responds first, discard the rest |
| `flat_map<In,Out>(f)` | filter | Map to sub-streams, merge results |
| `foreach_emit<T,S,U>(init,update,extract)` | filter | Generalized scan: separate state update and extraction |
| `flatten<T>` | filter | Flatten `vector<T>` → T |
| `gate<T>()` | function | Pause/resume via control channel |
| `group_by<T,K>(f)` | producer | Partition by key, emit (key, reader) pairs |
| `interleave<T>(readers...)` | producer | Strict round-robin interleave |
| `join<T>(readers...)` | function | Block until all channels close |
| `killswitch<T>()` | filter | Forward until keepalive dies |
| `last<T>(n)` | filter | Buffer; emit last n on close |
| `latch<T>()` | filter | Serve most recent value on demand |
| `io::lines(fd)` | producer | fd → `reader<string>` via `byte_reader \| split_lines` |
| `map<In,Out>(f)` | filter | Transform each element |
| `merge<T>(readers...)` | producer | Non-deterministic merge |
| `mux(reader<Ts>...)` | producer | Heterogeneous merge into `variant<Ts...>` |
| `demux(reader<variant<Ts...>>)` | function | Split variant stream into N typed readers |
| `metrics<T>()` | function | Passthrough with stats reporting |
| `mute<T>()` | producer | Never-producing endpoint |
| `normal(mean,stddev)` | producer | Normally distributed values |
| `nwise<T>(n)` | filter | Sliding n-element window as tuple |
| `pace<T>(trigger)` | filter | Rate-limited passthrough: one value per trigger, backpressure on excess |
| `pairwise<T>` | filter | Consecutive pairs (a,b), (b,c)... |
| `parallel_map<A,B>(n,f,cfg)` | filter | Concurrent N-worker transform; `cfg.ordered` preserves input order |
| `partition<T>(n,f)` | function | Route to N outputs by classifier |
| `quantize<T>(f)` | function | Variable-size batching |
| `reduce<T,A>(init,f)` | filter | Fold to single value |
| `round_robin<T>(n)` | function | Distribute across N outputs |
| `rpc_client` | function | Request/reply client: channel-pair variant (separate req/rep channels) or embedded-reply variant (uses `request<Req,Resp>` pattern) |
| `rpc_server` | function | Request/reply server: channel-pair variant or embedded-reply variant (uses `request<Req,Resp>` pattern) |
| `sample<T,S>(trigger)` | producer | Emit latest value on trigger |
| `scan<In,Out>(init,f)` | filter | Running accumulator |
| `share<T>(n)` | producer | Multicast with latch semantics |
| `shuffle<T>(n)` | filter | Reservoir shuffle through a bounded buffer |
| `sink<T>(f)` | consumer | Consume with side-effect function |
| `skip_first<T>(n)` | filter | Drop first n elements |
| `skip_last<T>(n)` | filter | Emit all but last n |
| `skip_while<T>(pred)` | filter | Drop while predicate true |
| `slide<T>(params)` | function | Sliding window with expiry |
| `stride<T>(n)` | filter | Every Nth element |
| `sort_merge<T>(readers,cmp)` | producer | Merge N pre-sorted streams into one sorted output |
| `switch_all<T>` | filter | Flatten sub-streams with latest-wins cancellation |
| `take_until<T>(pred)` | filter | Forward until predicate true (inclusive — emits the match) |
| `take_while<T>(pred)` | filter | Forward while predicate true |
| `tee<T>(side_writer)` | filter | Duplicate: main first, then side |
| `throttle<T>(trigger,cfg)` | filter | Rate-limit: `cfg.n` per trigger, use with `tick(d)` |
| `timeout<T>(dur)` | filter | Close if no value within duration |
| `timer(control)` | producer | Sleep per control, emit fire times |
| `transpose<T>(readers)` | producer | Dynamic-width zip: N readers in lockstep as `vector<T>` |
| `try_map<A,B>(f,err)` | filter | Map with exception catching; errors to side channel |
| `uniform_int<T>(lo,hi)` | producer | Uniform random integers in [lo, hi] |
| `uniform_real<T>(lo,hi)` | producer | Uniform random reals in [lo, hi) |
| `unique<T>(cap)` | filter | All-time dedup with optional eviction |
| `unzip<Tuple>()` | function | Split tuple stream into N streams |
| `where<T>(pred)` | filter | Filter by predicate |
| `window<T>(n)` | filter | Sliding window as `vector<T>` |
| `zip<Ts...>(readers...)` | producer | Element-wise zip |

## Gotchas

1. **Move-only endpoints**: `writer<T>` and `reader<T>` have deleted copy
   constructors. Use `std::move()` when passing to spawn/lambdas. Use
   `.copy()` for deliberate shared ownership.

2. **`after()` is non-negative**: `after(d)` sends a `time_point` before
   dying. In prialt, the timeout case is a non-negative match (e.g.,
   `case 1:`), not a death event (`case ~1:`).

3. **`~endpoint` returns complemented**: Death-watch operations (`~w`, `~r`)
   return complemented indices (`~k`) when they fire, just like implicit death
   on regular read/write ops. All death events are complemented.

4. **chan_op blocks in destructor**: `w << val;` as a statement blocks
   because `chan_op`'s destructor calls `prialt`. To avoid blocking, capture
   the return: `auto op = w << val; op.disarm();`.

5. **`spawn(f)` takes f by value**: The callable is moved into the
   imp. Ensure captured state is either moved or intentionally
   shared (via `shared_ptr` or `.copy()`).

6. **Reader range-for copies**: `for (T v : reader)` copies each value.
   Use `for (T& v : reader)` only for const access (iterator stores T).

7. **M:N runtime is the default**: The runtime auto-initializes with
   hardware concurrency. Use `set_maxprocs(1)` or `CSP_MAXPROCS=1` for
   single-threaded mode.

8. **Part spawn() consumes endpoints**: `filter.spawn(std::move(r))`
   takes the reader by value. Forgetting `std::move()` won't compile.

9. **stream_to checks writer death**: `r.stream_to(w)` uses
   `prialt(~out, in >> t)` internally, so it stops when the writer's
   reader dies — not just when the input reader dies.

10. **Dynamic bindings must use `local`**: `var = val` returns a deferred
    binding, not a mutation. Always wrap in `csp::local l{var = val}`.
    Bare `var = val;` asserts in debug builds.

11. **Dynamic scoping is per-imp**: Bindings via `local` use
    COW (path-copy HAMT). Changes are invisible to other imps
    unless explicitly shared via `context::current()` + `context_scope`.

## Integration

CSP distributes as a small set of vendor-drop-in files. The **core** trio is
always required:

| File | Purpose |
|---|---|
| `csp.h` | Single header (all public API) |
| `csp.cpp` | Core implementation + context-switching assembly |
| `csp_globals.cpp` | Thread-local state (**must** be a separate translation unit — see [docs/tls-caching-bug.md](https://github.com/marcelocantos/csp/blob/master/docs/tls-caching-bug.md)) |
| `AGENTS-CSP.md` | This file — agent reference for CSP |

Each network protocol ships as its own optional drop-in `.cpp` file. Compile
**only** the protocols you use:

| File | Protocol | Vendored library required at link |
|---|---|---|
| `csp_tls.cpp` | TLS 1.3 | PicoTLS + minicrypto (build with `-DCSP_TLS`) |
| `csp_http.cpp` | HTTP/1.1 | llhttp |
| `csp_http2.cpp` | HTTP/2 | nghttp2 (PicoTLS too for `serve_tls`) |
| `csp_ws.cpp` | WebSocket | wslay (HTTP/1.1 too via `csp_http.cpp` for `upgrade`) |
| `csp_quic.cpp` | QUIC | ngtcp2 + PicoTLS + `ngtcp2_crypto_picotls_minicrypto.c` |
| `csp_http3.cpp` | HTTP/3 | nghttp3 + everything QUIC needs |

The single header `csp.h` declares **all** protocol APIs. Per-protocol `.cpp`
files cherry-pick which implementations the linker keeps. Skip a `.cpp`
file and its third-party deps drop out of the link (provided you have
linker dead-code elimination enabled — see below).

### Fetching the vendored libraries

CSP ships
[`scripts/vendor-deps.sh`](https://github.com/marcelocantos/csp/blob/master/scripts/vendor-deps.sh)
to fetch known-good versions of the third-party libraries the per-protocol
drop-ins need. Drop the script into your project alongside `dist/` and run:

```bash
./scripts/vendor-deps.sh --all                 # every library
./scripts/vendor-deps.sh --http --ws           # HTTP/1.1 + WebSocket
./scripts/vendor-deps.sh --http3               # HTTP/3 (pulls QUIC + TLS)
```

The script clones each library to `vendor/<name>/` at a pinned commit and
generates the version headers (`nghttp2ver.h`, `version.h`, `wslayver.h`)
that are normally produced by autotools/cmake. The include paths in the
compile recipes below match what the script produces.

### Compile recipes

Compile with C++20 and libc++. Channels-only:

```bash
c++ -std=c++20 -O2 -ffunction-sections -fdata-sections \
    -c csp.cpp csp_globals.cpp
c++ -Wl,-dead_strip   csp.o csp_globals.o -o app    # macOS / ld64
c++ -Wl,--gc-sections csp.o csp_globals.o -o app    # Linux / lld / GNU ld
```

Channels + HTTP/1.1:

```bash
c++ -std=c++20 -O2 -ffunction-sections -fdata-sections \
    -I vendor/llhttp/include \
    -c csp.cpp csp_globals.cpp csp_http.cpp \
       vendor/llhttp/src/{llhttp,api,http}.c
c++ -Wl,-dead_strip csp.o csp_globals.o csp_http.o llhttp.o api.o http.o -o app
```

Channels + HTTPS (TLS + HTTP/1.1):

```bash
c++ -std=c++20 -O2 -DCSP_TLS -ffunction-sections -fdata-sections \
    -I vendor/picotls/include -I vendor/llhttp/include \
    -c csp.cpp csp_globals.cpp csp_tls.cpp csp_http.cpp \
       vendor/picotls/lib/*.c vendor/llhttp/src/{llhttp,api,http}.c
c++ -Wl,-dead_strip csp.o csp_globals.o csp_tls.o csp_http.o *_picotls*.o *llhttp*.o -o app
```

QUIC needs the extra C99 adapter:

```bash
cc  -std=c99 -O2 -I vendor/ngtcp2/lib -I vendor/picotls/include \
    -c ngtcp2_crypto_picotls_minicrypto.c
c++ -std=c++20 -O2 -DCSP_TLS -ffunction-sections -fdata-sections \
    -I vendor/picotls/include -I vendor/ngtcp2/lib/includes \
    -c csp.cpp csp_globals.cpp csp_tls.cpp csp_quic.cpp \
       vendor/picotls/lib/*.c vendor/ngtcp2/lib/*.c
```

### Linker dead-code elimination

The per-protocol model relies on three flags:

* `-ffunction-sections -fdata-sections` (compile-time): emit each function
  and variable in its own section so the linker can drop them individually.
* `-Wl,-dead_strip` (macOS / ld64) **or** `-Wl,--gc-sections` (Linux / lld /
  GNU ld) (link-time): drop sections (and TUs in static archives) that no
  symbol in the link line references.

With these flags, a binary that calls only `csp::http::serve` keeps only
the http functions it actually uses; the rest of `csp_http.cpp` and the
unused parts of `llhttp` are stripped. Protocols you don't compile in
contribute zero bytes.

The model is contractual on five rules — `csp.cpp` (the core TU) does **not**
reference any `csp::tls::`, `csp::http::`, `csp::http2::`, `csp::http3::`,
`csp::ws::`, or `csp::quic::` symbol. Calling a protocol's namespace is the
only way to make its TU live. See
[`docs/design/per-protocol-dist.md`](https://github.com/marcelocantos/csp/blob/master/docs/design/per-protocol-dist.md)
for the full discussion.

There are two ways to make a protocol live:

1. **Direct call** — `csp::http::serve(8080, {...})` etc. The existing
   API; what every example in this document uses. Each direct call
   pulls its protocol's TU into the link automatically.
2. **Factory API** (🎯T23.1) —
   `csp::net::serve(8080, {csp::http::enable()})`. Each protocol provides
   an `enable()` factory returning a `csp::net::protocol_option` carrying
   a function-pointer `apply` that the unified `csp::net::serve` walks.
   The front-door TU sees only the function pointer, so adding a new
   protocol doesn't add any name-level dependency to `csp.cpp`.

The two are interchangeable for single-protocol cases; the factory form
exists primarily so cross-protocol composition (ALPN-negotiated HTTP/1.1
vs HTTP/2 on a TLS socket, etc.) has a single entry point. Current Phase B
ships with `csp::http::enable()` implemented; other protocols' factories
land as needed.

The contract is enforced in CI by
[`scripts/subset_check.sh`](https://github.com/marcelocantos/csp/blob/master/scripts/subset_check.sh)
(🎯T23.3), which builds subset configurations (channels-only, http-only,
http+ws, quic-only, full) on macOS arm64 and Linux x86_64. Each job runs
`vendor-deps.sh` for that subset's libraries, compiles the drop-in plus a
small sample, links with `-dead_strip` / `--gc-sections`, and uses `nm` to
assert that libraries belonging to unselected protocols (`llhttp_`,
`nghttp2_`, `nghttp3_`, `ngtcp2_`, `wslay_`, `ptls_`) are absent from the
final binary. A regression that pulls an unselected protocol's symbols into
the front-door TU trips a clear, named CI failure.

Reference this file from your project's `CLAUDE.md` or `AGENTS.md` to
give coding agents CSP expertise.
