# rpc

Request-response communication over CSP channels. Two variants are provided:
**channel-pair RPC** (separate request and reply channels) and **reply-in-request
RPC** (reply channel embedded in each request). Both variants produce callable
objects on the client side and spawnable functions on the server side.

## Signatures

### Channel-pair variant

The client sends requests on one channel and reads replies on another. The
server must deliver each reply before accepting the next request.

```cpp
template <typename... Args, typename Rep>
auto rpc_client(writer<std::tuple<Args...>> req, reader<Rep> rep);
// Returns: callable (Args...) -> Rep

template <typename... Args, typename Rep, typename F>
auto rpc_server(reader<std::tuple<Args...>> req, writer<Rep> rep, F&& f);
// Returns: spawnable function () -> void
```

### Reply-in-request variant

Each request carries a fresh reply channel. The server may accept new requests
while earlier replies are still in flight.

```cpp
template <typename... Args, typename Rep>
auto rpc_client(writer<std::pair<std::tuple<Args...>, writer<Rep>>> req);
// Returns: callable (std::tuple<Args...>) -> Rep

template <typename... Args, typename Rep, typename F>
auto rpc_server(reader<std::pair<std::tuple<Args...>, writer<Rep>>> req, F&& f);
// Returns: spawnable function () -> void
```

## Topology

### Channel-pair

<!-- csp-flow
          -"tuple<Args...>"->
{client}                      {server}
          <-"Rep"-
-->
![rpc topology](diagrams/rpc_pair.svg)

The client and server share a matched pair of channels. The server processes
one request at a time: it reads a request, applies `f`, writes the reply, then
loops.

### Reply-in-request

<!-- csp-flow
          -"pair<tuple<Args...>, writer<Rep>>"->
{client}                                        {server}
          <-"Rep"-
-->
![rpc topology](diagrams/rpc_reply.svg)

Each call creates a fresh `chan<Rep>`. The client sends the request arguments
together with the write end of the reply channel, then blocks on the read end.
The server writes the reply to the embedded writer, which may happen
immediately or after accepting further requests.

## Semantics

### Channel-pair

- `rpc_client` returns a callable that sends `std::tuple<Args...>` on `req`
  and blocks for a reply on `rep`. If the reply channel dies before the
  request is sent (detected via `alt`), the client throws
  `std::runtime_error("rpc dead")`.
- `rpc_server` returns a nullary function suitable for `spawn`. It loops,
  reading requests and writing replies. It exits when either the request
  channel is exhausted or the reply channel's reader is dropped.
- The server is **sequential**: it must write a reply before reading the next
  request. This makes the channel-pair variant simple but limits concurrency.

### Reply-in-request

- `rpc_client` returns a callable that takes a `std::tuple<Args...>`,
  creates a fresh `chan<Rep>`, sends the pair, and blocks for the reply. If
  the request channel is dead, the client throws
  `std::runtime_error("rpc dead")`.
- `rpc_server` returns a nullary function suitable for `spawn`. It loops,
  reading request pairs and writing replies to the embedded writer. It exits
  when the request channel is exhausted.
- The server **may overlap** with concurrent clients because each reply goes
  to an independent channel. However, the single-threaded server function
  still processes requests sequentially; true parallelism requires spawning
  the handler.

### Void arguments and void replies

- **No arguments**: use `std::tuple<>` as the request type. The handler `f`
  takes no parameters.
- **No return value**: use `chan<>` (i.e. `poke_t`) as the reply type. The
  handler `f` returns void; the RPC machinery automatically converts this to
  a `poke` acknowledgment.

## Example

### Basic channel-pair RPC

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

auto [req_w, req_r] = chan<std::tuple<int>>{};
auto [rep_w, rep_r] = chan<int>{};

// Server: doubles and adds one.
spawn(rpc_server(std::move(req_r), std::move(rep_w),
    [](int n) { return 2 * n + 1; }));

// Client: returns a callable int -> int.
auto f = rpc_client(std::move(req_w), std::move(rep_r));

f(0);   // 1
f(10);  // 21
f(7);   // 15
```

### Void request and reply (notification / ping)

```cpp
auto [req_w, req_r] = chan<std::tuple<>>{};
auto [rep_w, rep_r] = chan<>{};

int count = 0;
spawn(rpc_server(req_r.copy(), rep_w.copy(),
    [&count]{ ++count; }));

auto ping = rpc_client(req_w.copy(), rep_r.copy());
ping();  // count is now 1
ping();  // count is now 2
```

### Reply-in-request (per-call reply channel)

```cpp
auto [req_w, req_r] =
    chan<std::pair<std::tuple<int>, writer<int>>>{};

spawn(rpc_server(req_r.copy(),
    [](int n) { return 2 * n + 1; }));

auto f = rpc_client(req_w.copy());

f(std::tuple{0});   // 1
f(std::tuple{10});  // 21
```

## See Also

- [buffer](buffer.md) -- decouple producer and consumer with a FIFO buffer
- [fanout](fanout.md) -- broadcast one writer to many readers
