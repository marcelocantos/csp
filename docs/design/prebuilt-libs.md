# Pre-built library artefacts

CSP ships as vendor-drop-in source (`dist/`), but every GitHub Release also
publishes **pre-built libraries** so downstream projects can link against
compiled archives without compiling CSP or its vendored third-party
dependencies themselves. This page documents what is published, how to link
it, and the ABI policy. It is the source material for each release's notes.

## What is published

Per supported platform, a `csp-<version>-<platform>.tar.gz` containing static
(`.a`) and shared (`.dylib`/`.so`) libraries:

| Library | Contents |
|---|---|
| `libcsp` | Core: channels, scheduler, I/O reactor, timers (no protocols) |
| `libcsp_tls` | TLS 1.3 (PicoTLS) |
| `libcsp_http` | HTTP/1.1 |
| `libcsp_http2` | HTTP/2 |
| `libcsp_http3` | HTTP/3 |
| `libcsp_ws` | WebSocket |
| `libcsp_quic` | QUIC |
| `lib<dep>` | Vendored deps: `libllhttp`, `libpicotls`, `libnghttp2`, `libnghttp3`, `libngtcp2`, `libwslay` |

The one-library-per-protocol split preserves the per-protocol cherry-pick
model of the source drop-in (see
[per-protocol-dist.md](per-protocol-dist.md)): link only the protocols you
use, and the linker's dead-code elimination drops the rest.

A single platform-independent `csp-<version>-include.tar.gz` carries the
public header. CSP's public API is one amalgamated header, `csp.h`, that
declares **all** protocol APIs — there are no separate per-protocol headers to
manage. Selecting protocols happens at link time (which `libcsp_<proto>` you
link), not at include time.

## Supported platforms

| Platform | Triple | Shared ext | C++ runtime |
|---|---|---|---|
| macOS arm64 | `arm64-apple-darwin` | `.dylib` | libc++ |
| Linux x86_64 | `x86_64-linux-gnu` | `.so` | libstdc++ ABI (built with clang+libc++; see ABI policy) |
| Linux arm64 | `aarch64-linux-gnu` | `.so` | as above |

## Link incantation

Compile your code with **C++20 and libc++**, point the include path at the
unpacked header, and link the core lib + each protocol lib + its vendored dep.
Add the platform dead-strip flag so unreferenced protocol code drops out.

The artefacts are built with `-ffunction-sections -fdata-sections`, so the
final-app dead-strip works exactly as it does for a source build.

### macOS arm64

```bash
c++ -std=c++20 -stdlib=libc++ -O2 -I include \
    my_app.cpp \
    -L lib -lcsp_http -lcsp -lllhttp \
    -Wl,-dead_strip -Wl,-rpath,$PWD/lib -o my_app
```

### Linux x86_64 / arm64

```bash
clang++-18 -std=c++20 -stdlib=libc++ -O2 -I include \
    my_app.cpp \
    -L lib -lcsp_http -lcsp -lllhttp \
    -Wl,--gc-sections -Wl,-rpath,$ORIGIN -o my_app
```

The library list per use case (core is always required):

| Use case | Libraries |
|---|---|
| Channels only | `-lcsp` |
| HTTP/1.1 | `-lcsp_http -lcsp -lllhttp` |
| HTTP/1.1 + TLS | `-lcsp_http -lcsp_tls -lcsp -lllhttp -lpicotls` |
| HTTP/2 | `-lcsp_http2 -lcsp_tls -lcsp -lnghttp2 -lpicotls` |
| WebSocket | `-lcsp_ws -lcsp_http -lcsp -lwslay -lllhttp` |
| QUIC | `-lcsp_quic -lcsp_tls -lcsp -lngtcp2 -lpicotls` |
| HTTP/3 | `-lcsp_http3 -lcsp_quic -lcsp_tls -lcsp -lnghttp3 -lngtcp2 -lpicotls` |

List CSP libraries before their vendored deps (`libllhttp`, `libpicotls`,
…) — GNU ld resolves symbols left-to-right, so a library must precede the
archives that satisfy it. The `.a` and `.so`/`.dylib` of each library carry
identical symbols; the linker picks the shared variant by default. To force a
fully static binary, pass the `.a` paths explicitly instead of `-l`.

For TLS-gated protocols (`tls`, `http2`, `http3`, `quic`) define `-DCSP_TLS`
when compiling your own translation units, matching how the libraries were
built.

## ABI policy

- **libc++ is the supported C++ runtime** on all platforms. The published
  libraries are compiled with clang + libc++ and expose a libc++ C++ ABI.
  Link your application with `-stdlib=libc++`.
- **libstdc++ users must build from source.** Mixing a libc++-built CSP with
  a libstdc++ application is not ABI-safe (different `std::string`,
  `std::exception`, etc. layouts). If your toolchain or wider dependency set
  pins you to libstdc++, take the per-protocol source drop-in (`dist/`) and
  compile it into your build with your own standard library — that path is
  exactly what the source distribution exists for. See
  [per-protocol-dist.md](per-protocol-dist.md).
- The libraries are unversioned (no SONAME version yet); treat each release's
  artefacts as a matched set and rebuild downstream against a new release.

## Out of scope (v1)

- **Windows** and **mobile (iOS/Android)** platforms ship no pre-built
  artefacts in v1. CSP's source still builds on Windows (CMake, see CI), and
  the source drop-in is the supported path there: vendor `dist/` and compile
  it into your own build.
- Only the three platforms in the table above are published. Anything else is
  a source build.

## How the artefacts are produced

[`scripts/build-libs.sh`](../../scripts/build-libs.sh) compiles each protocol
drop-in into its own `libcsp_<proto>` and each vendored library into
`lib<dep>`, all from `dist/`, fetching the third-party deps via
[`scripts/vendor-deps.sh`](../../scripts/vendor-deps.sh). `make libs` runs it
for the host platform; `make downstream-test` then links the
[downstream sample](../../examples/downstream/downstream.cc) against the
output to prove the link incantation. The
[release workflow](../../.github/workflows/release.yml) runs both across the
three platforms and uploads the tarballs to the release.
