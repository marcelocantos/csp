#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace csp::file {

// Read an entire file into memory. Offloads the blocking read to the
// blocking thread pool so the calling imp suspends cooperatively.
// Throws csp::error on failure.
[[nodiscard]] std::vector<uint8_t> read(const std::string& path);

// Write data to a file (creates or overwrites). Offloads the blocking
// write to the blocking thread pool.
// Throws csp::error on failure.
void write(const std::string& path, const std::vector<uint8_t>& data);
void write(const std::string& path, const void* data, size_t len);

}
