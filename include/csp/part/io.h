#pragma once

#include <csp/io.h>
#include <csp/part/part.h>

#include <string>
#include <vector>

namespace csp::part::io {

// Produce byte chunks from a non-blocking fd/socket. Each message
// contains as much data as was available from a single read() call.
// Owns the fd and closes it on exit.
inline auto byte_reader(csp::io::socket_t fd, size_t chunk_size = 4096) {
    return make_producer<bytes>(
        [fd, chunk_size](writer<bytes> out) {
            internal::descr("byte_reader");
            csp::io::set_nonblock(fd);

            bytes buf(chunk_size);
            for (;;) {
                ssize_t n = csp::io::read(fd, buf.data(), buf.size());
                if (n <= 0) break;
                buf.resize(static_cast<size_t>(n));
                if (!(out << std::move(buf))) break;
                buf.resize(chunk_size);
            }
            csp::io::close(fd);
        });
}

// Consume byte chunks and write them to an fd/socket. Owns the fd
// and closes it on exit.
inline auto byte_writer(csp::io::socket_t fd) {
    return make_consumer<bytes>(
        [fd](reader<bytes> in) {
            internal::descr("byte_writer");
            csp::io::set_nonblock(fd);

            for (bytes chunk; in >> chunk;) {
                if (csp::io::write(fd, chunk.data(), chunk.size()) < 0) break;
            }
            csp::io::close(fd);
        });
}

// Split a byte stream into lines (LF-delimited). Pure channel
// transform — no I/O knowledge, testable with synthetic data.
// Flushes any partial trailing line (no trailing newline) on input
// close.
inline auto const split_lines = make_filter<bytes, std::string>(
    [](reader<bytes> in, writer<std::string> out) {
        internal::descr("split_lines");

        std::string pending;
        for (bytes chunk;
             csp::alt(in >> chunk, ~out) >= 0;) {
            size_t start = 0;
            for (size_t i = 0; i < chunk.size(); ++i) {
                if (chunk[i] == '\n') {
                    pending.append(
                        reinterpret_cast<const char*>(chunk.data()) + start,
                        i - start);
                    if (!(out << std::move(pending))) return;
                    pending.clear();
                    start = i + 1;
                }
            }
            if (start < chunk.size()) {
                pending.append(
                    reinterpret_cast<const char*>(chunk.data()) + start,
                    chunk.size() - start);
            }
        }
        if (!pending.empty()) {
            out << std::move(pending);
        }
    });

// Split a byte stream into fixed-size frames. Discards any partial
// trailing frame on input close.
inline auto fixed_frames(size_t frame_size) {
    return make_filter<bytes>(
        [frame_size](reader<bytes> in,
                     writer<bytes> out) {
            internal::descr("fixed_frames");

            bytes frame;
            frame.reserve(frame_size);
            for (bytes chunk;
                 csp::alt(in >> chunk, ~out) >= 0;) {
                size_t i = 0;
                while (i < chunk.size()) {
                    size_t need = frame_size - frame.size();
                    size_t avail = chunk.size() - i;
                    size_t take = std::min(need, avail);
                    frame.insert(frame.end(),
                                 chunk.begin() + i,
                                 chunk.begin() + i + take);
                    i += take;
                    if (frame.size() == frame_size) {
                        if (!(out << std::move(frame))) return;
                        frame.clear();
                        frame.reserve(frame_size);
                    }
                }
            }
        });
}

}
