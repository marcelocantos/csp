#pragma once

// Pull-based sized-read abstraction (🎯T17 Stage 1).
//
// A source is a channel into which consumers submit read requests.
// Each request carries the desired byte count and a one-shot reply
// channel.  The source imp fills the request (up to N bytes) and writes
// the result into the reply channel.  EOF, errors, and normal data each
// travel on structurally distinct paths:
//
//   - Success:  the reply channel carries a bytes value.
//   - EOF:      the source imp exits; the reply-writer drops; the
//               consumer's reader<bytes> observes peer death (>> returns
//               false).  Structural — no sentinel value needed.
//   - Errors:   the source calls req.reply._throw(ep) before exiting;
//               the consumer's >> rethrows at the call site.
//
// This file defines the type aliases and the fd_source factory
// (Stage 1).  Higher layers (tls_source, http_body_source) are in
// separate headers.

#include <csp/csp.h>
#include <csp/internal/on_scope_exit.h>
#include <csp/io.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace csp::io {

// --- Core type aliases ---

// A single pull request: consumer asks for up to `value` bytes;
// the source writes up to that many into the `reply` channel.
using read_request = request<size_t, bytes>;

// The consumer-facing handle.  A source is the *write* end of a
// request channel — consumers write requests into it and read
// replies from the one-shot reply channels embedded in each request.
using source = writer<read_request>;

// --- errno_error: carry a syscall name + errno across channels ---

class errno_error : public csp::error {
public:
    errno_error(const std::string& syscall, int err)
        : csp::error(syscall + ": " + std::strerror(err)), err_(err) {}

    int err() const { return err_; }

private:
    int err_;
};

// --- fd_source: TCP / pipe source (Stage 1 concrete implementation) ---
//
// Spawns an imp that serves read_request values from a non-blocking fd.
// Contract: up to N bytes per request, matching read(2) semantics.
// Partial reads (m < N) are normal; the consumer decides whether to
// re-request.
//
// Lifecycle:
//   - EOF (io::read returns 0): imp exits without writing a reply.
//     The reply-writer drops; the consumer's reader sees peer death.
//   - Error (io::read returns < 0): imp throws errno_error across the
//     current reply via _throw, then exits.  The consumer's >> rethrows.
//   - Zero-byte request: imp throws std::invalid_argument and exits.
//     (Almost always a caller bug; we surface it immediately.)
//
// The source owns the fd and closes it when the imp exits (normal,
// EOF, or error).

namespace detail {

// Shared implementation for fd_source (owning) and fd_source_view
// (non-owning).  The owning variant installs an OnScopeExit guard that
// closes the fd on every imp exit path (EOF, error, zero-byte request,
// or request-channel drop).
[[nodiscard]] inline source make_fd_source(fd_t fd, bool owned,
                                           char const* name) {
    chan<read_request> ch;

    spawn([req_r = std::move(ch.r), fd, owned, name]() mutable {
        internal::descr(name);
        assert(fd.is_nonblock() && "fd_source: fd must be non-blocking");

        auto close_guard = onScopeExit([fd, owned] {
            if (owned) csp::io::close(fd);
        });

        read_request req;
        while (req_r >> req) {
            if (req.value == 0) {
                // Almost always a caller bug; surface it immediately.
                req.reply._throw(
                    std::make_exception_ptr(std::invalid_argument(
                        std::string(name) + ": zero-byte read request")));
                return;
            }

            bytes buf(req.value);
            ssize_t n = csp::io::read(fd, buf.data(), buf.size());

            if (n == 0) {
                // EOF: exit without writing.  The reply-writer drops, and
                // the consumer's reader<bytes> sees peer death.
                return;
            }

            if (n < 0) {
                // Error: throw across the reply, then exit.
                int err = errno;
                req.reply._throw(
                    std::make_exception_ptr(errno_error("read", err)));
                return;
            }

            buf.resize(static_cast<size_t>(n));
            req.reply << std::move(buf);
        }

        // Request channel closed by consumer — clean shutdown.
    });

    return std::move(ch.w);
}

} // namespace detail

[[nodiscard]] inline source fd_source(fd_t fd) {
    return detail::make_fd_source(fd, /*owned=*/true, "fd_source");
}

// --- fd_source_view: non-owning variant ---
//
// Same protocol as fd_source, but the imp does NOT close the fd on
// exit (EOF, error, or request-channel drop).  The caller owns the
// fd's lifecycle.  Useful when the fd must outlive the source — e.g.
// the HTTP server's connection handler needs the fd available for
// the WebSocket-upgrade hijack handoff after dropping the source.

[[nodiscard]] inline source fd_source_view(fd_t fd) {
    return detail::make_fd_source(fd, /*owned=*/false, "fd_source_view");
}

// --- Convenience helpers for source consumers ---

// Read exactly one request from a source and return the bytes.
// Throws on error; throws csp::channel_closed on EOF.
inline bytes source_read(source& s, size_t n) {
    return s(n);
}

// Non-blocking form: returns a reader<bytes> for the response.
// Use in prialt:
//   auto reply = csp::io::call_source(s, 4096);
//   prialt(reply >> buf, ~other);
inline reader<bytes> call_source(source& s, size_t n) {
    return call(s, n);
}

} // namespace csp::io
