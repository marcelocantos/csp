#pragma once

#include <csp/csp.h>

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
    size_t read(bytes& out);
};

}
