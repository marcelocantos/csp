# 35 — Non-channel performance surfaces (🎯T38)

*2026-07-20. Measurement platform: Apple M4 Max, macOS Darwin 25.5.0,
Clang -O2, `CSP_TLS` build. Driver: `examples/t38_surfaces.cc`. Channel
and scheduler hot path are **closed background** from
[paper 33](33-channel-hot-path-performance.md) (Rounds 1–7, 🎯T34–T37);
this paper does not re-litigate send/recv flatness, alt/8ch thrash, or
the multi-writer cache-line floor.*

## Why this paper

After T37 the paper-33 opportunity list is exhausted for channel/M:N
scheduling. Residual performance risk, if any, lives on **other
surfaces**: reactor I/O, HTTP/TLS protocol stacks, stack pool / spawn,
cross-OS differences, and dist-consumer LTO. 🎯T38 ranks those surfaces
from measurement and code inspection — investigation only, no optimisations.

## Method

| Tool | Use |
|---|---|
| `examples/t38_surfaces.cc` | Chrono microbenches: spawn, net echo, HTTP GET, channel reference |
| Code inspection | `src/reactor.cc`, `src/stack_pool.cc`, `src/tls.cc`, `src/http*.cc` |
| Paper 33 Round 4 | ThinLTO A/B already recorded (not re-run) |
| Linux host | **Not available** this session (Darwin only) — see §Linux-vs-macOS |

Re-run measurements:

```bash
make build/normal/examples/t38_surfaces
CSP_MAXPROCS=2 ./build/normal/examples/t38_surfaces
CSP_MAXPROCS=16 ./build/normal/examples/t38_surfaces
```

## Headline numbers (M4 Max, 2026-07-20)

| Shape | MAXPROCS=2 | MAXPROCS=16 | Notes |
|---|---:|---:|---|
| `channel/send-recv` (reference) | **149 ns** | **150 ns** | Flat — paper 33 / T34 still holds |
| `spawn/empty` | 1.76 µs | 2.90 µs | Stack pool + fcontext + schedule |
| `spawn/yield` | 1.95 µs | 2.49 µs | + one yield pair |
| `net/echo-rtt` (listen+dial+rw) | 225 µs | 179 µs | Full setup per op |
| `net/echo-steady` (dial+rw) | **89 µs** | **86 µs** | Shared listener |
| `http/get-setup` (serve+GET) | 220 µs | 221 µs | One accept+request per op |

Order of magnitude: **channel ~0.15 µs ≪ spawn ~2–3 µs ≪ net/HTTP ~90–220 µs**.
I/O and protocol setup dominate any remaining scheduler cost on networked paths.

## Surfaces

### Reactor / I/O

| Field | Content |
|---|---|
| **Hotspot** | Kernel path: `kqueue`/`kevent` (macOS) or `epoll` (Linux) in `src/reactor.cc`; non-blocking fd register/unregister; `net::dial`/`listen` + `io::call_source` for byte transfer |
| **Magnitude** | Steady localhost echo **~86–89 µs/op**; full listen+dial setup **~180–225 µs/op**. ~500–1500× a channel rendezvous |
| **Method** | `t38_surfaces` net shapes; code inspection of reactor backend split |
| **Verdict** | **no action** for CSP-layer micro-opts. Cost is syscall + TCP stack. An **architectural follow-up** (io_uring, batch accept) would be a product decision, not a bugfix — not filed |
| **Follow-up** | none |

DNS resolution still uses the blocking pool (`io::resolve`); that is intentional and off the hot transfer path once connected.

### HTTP / TLS (and related protocol TUs)

| Field | Content |
|---|---|
| **Hotspot** | HTTP/1.1: `http::serve` + llhttp parse + response write (`src/http.cc`). TLS: PicoTLS handshake/record in `src/tls.cc` (crypto-bound). HTTP/2/3, WS, QUIC: large vendored stacks (nghttp2, nghttp3, wslay, ngtcp2) behind drop-in TUs |
| **Magnitude** | In-process HTTP GET **~220 µs/op** (serve + one request). No TLS handshake timed this session (would be ms-scale crypto + cert I/O). Protocol TUs: ~6.5k LOC including vendored glue — cost is feature surface, not an unprofiled CSP scheduler leak |
| **Method** | `t38_surfaces` http shape; code inspection of `http.cc` / `tls.cc` / per-protocol dist model (🎯T23) |
| **Verdict** | **no action** on the CSP runtime. HTTP setup is already an order of magnitude above net steady-state; further wins are protocol/product (keep-alive pooling, H2 prioritization), not M:N tweaks. TLS remains crypto-bound |
| **Follow-up** | none |

### Stack pool / spawn path

| Field | Content |
|---|---|
| **Hotspot** | `StackPool::arena_alloc` / `arena_free` under a **global `mu_`** (`src/stack_pool.cc`); fcontext make; imp construct; global spawn → schedule |
| **Magnitude** | Empty spawn **1.76 µs @ 2p → 2.90 µs @ 16p** (~1.6×). Still ~20× a rendezvous, **~30× cheaper than net steady echo** |
| **Method** | `t38_surfaces` spawn shapes; code inspection confirms global free-list lock |
| **Verdict** | **micro-opt opportunity** (sharded free lists / per-P cache) if a workload is spawn-storm dominated. **Not filed as a target**: absolute cost is small vs I/O; 16p slowdown is modest; papers 5/20/25 already own stack engineering. Revisit only with a product spawn-rate requirement |
| **Follow-up** | none (docs-only note) |

### Linux vs macOS

| Field | Content |
|---|---|
| **Hotspot** | Reactor backend (epoll vs kqueue); futex vs `__ulock_*` Notes; stack arena `mmap` flags |
| **Magnitude** | **Not measured here** — this host is Darwin only. No Linux machine in the 🎯T38 session |
| **Method** | Honest skip. Prior Linux-specific work lives in paper 27 (web crawler hang) and CI, not a same-day A/B |
| **Verdict** | **no action** pending a Linux sample. When Linux numbers exist, compare `t38_surfaces` on both and only open a target if a surface diverges by a large factor for the same workload |
| **Follow-up** | none |

### Dist-consumer LTO (documentation only)

| Field | Content |
|---|---|
| **Hotspot** | Whole-program optimise of amalgamated `dist/csp.cpp` vs multi-TU dev build |
| **Magnitude** | Paper 33 Round 4: ThinLTO on core TUs **~0–4%**, not pursued. Dist single-TU already gives consumers LTO-like visibility |
| **Verdict** | **no action** — user/dist documentation only; **not a bullseye target** |
| **Follow-up** | none |

## Summary table

| Surface | Verdict | Magnitude (order) | Target? |
|---|---|---|---|
| Reactor / I/O | no action | ~90–220 µs net/HTTP setup | no |
| HTTP / TLS | no action | ~220 µs GET; TLS crypto-bound | no |
| Stack pool / spawn | micro-opt opportunity | ~2–3 µs spawn; mild 16p contention | no (docs only) |
| Linux vs macOS | no action (unmeasured) | n/a this session | no |
| Dist LTO | no action (docs only) | ~0–4% ThinLTO | no |

**No large, actionable opportunities were filed.** Nothing in this portfolio is both (a) large relative to I/O-bound product paths and (b) a clear CSP implementation bug or cheap win. Architectural ideas (io_uring, spawn free-list sharding) stay notes until a product workload demands them.

## Relation to paper 33 / 🎯T34–T37

Channel `send/recv` remains **flat ~149–150 ns** at 2 and 16 procs in the same driver run — consistent with paper 33 after O1 wake-to-local. This paper does **not** reopen alt/8ch 2× acceptance, runnext, idle freelist, lazy pool sizing, or sticky affinity (those were investigated and deferred/rejected in session 2026-07-20).

## See also

- [33 — Channel hot-path performance](33-channel-hot-path-performance.md)
- [05 — Stack engineering](05-stack-engineering.md), [20 — Arena stack scaling](20-arena-stack-scaling.md)
- Driver: [`examples/t38_surfaces.cc`](../../examples/t38_surfaces.cc)
