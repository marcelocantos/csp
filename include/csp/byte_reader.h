#pragma once

#include <csp/csp.h>

#include <algorithm>
#include <cstring>

namespace csp {

// File-like byte stream interface over a reader<bytes>. Buffers
// partial chunks internally so callers can request exact byte counts.
class byte_reader {
    reader<bytes> r_;
    bytes buf_;
    size_t pos_ = 0;

public:
    explicit byte_reader(reader<bytes> r) : r_(std::move(r)) {}

    // Fill out with bytes pulled from the underlying reader.
    // Returns the number of bytes actually read. A return value less
    // than out.size() means the reader closed before the buffer could
    // be filled.
    size_t read(bytes& out) {
        size_t total = 0;
        size_t need = out.size();

        // Drain leftover from a previous call.
        if (pos_ < buf_.size()) {
            size_t avail = buf_.size() - pos_;
            size_t n = std::min(avail, need);
            std::memcpy(out.data(), buf_.data() + pos_, n);
            pos_ += n;
            total = n;
            if (pos_ == buf_.size()) {
                buf_.clear();
                pos_ = 0;
            }
            if (total == need) return total;
        }

        // Pull chunks from the underlying reader.
        bytes chunk;
        while (total < need && (r_ >> chunk)) {
            size_t n = std::min(chunk.size(), need - total);
            std::memcpy(out.data() + total, chunk.data(), n);
            total += n;
            if (n < chunk.size()) {
                buf_ = std::move(chunk);
                pos_ = n;
                return total;
            }
        }

        return total;
    }
};

}
