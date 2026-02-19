#pragma once

#include <csp/io.h>
#include <csp/part/part.h>

#include <string>
#include <vector>

namespace csp::part::io {

// Produce byte chunks from a non-blocking fd. Each message contains
// as much data as was available from a single read() call. Owns the
// fd and closes it on exit.
inline auto byte_reader(int fd, size_t chunk_size = 4096) {
    return make_producer<std::vector<uint8_t>>(
        [fd, chunk_size](writer<std::vector<uint8_t>> out) {
            internal::descr("byte_reader");
            csp::io::set_nonblock(fd);

            std::vector<uint8_t> buf(chunk_size);
            for (;;) {
                ssize_t n = csp::io::read(fd, buf.data(), buf.size());
                if (n <= 0) break;
                buf.resize(static_cast<size_t>(n));
                if (!(out << std::move(buf))) break;
                buf.resize(chunk_size);
            }
            ::close(fd);
        });
}

// Consume byte chunks and write them to an fd. Owns the fd and
// closes it on exit.
inline auto byte_writer(int fd) {
    return make_consumer<std::vector<uint8_t>>(
        [fd](reader<std::vector<uint8_t>> in) {
            internal::descr("byte_writer");
            csp::io::set_nonblock(fd);

            for (std::vector<uint8_t> chunk; in >> chunk;) {
                if (csp::io::write(fd, chunk.data(), chunk.size()) < 0) break;
            }
            ::close(fd);
        });
}

// Split a byte stream into lines (LF-delimited). Pure channel
// transform — no I/O knowledge, testable with synthetic data.
// Flushes any partial trailing line (no trailing newline) on input
// close.
inline auto const split_lines = make_filter<std::vector<uint8_t>, std::string>(
    [](reader<std::vector<uint8_t>> in, writer<std::string> out) {
        internal::descr("split_lines");

        std::string pending;
        for (std::vector<uint8_t> chunk;
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
    return make_filter<std::vector<uint8_t>>(
        [frame_size](reader<std::vector<uint8_t>> in,
                     writer<std::vector<uint8_t>> out) {
            internal::descr("fixed_frames");

            std::vector<uint8_t> frame;
            frame.reserve(frame_size);
            for (std::vector<uint8_t> chunk;
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
