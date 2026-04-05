#include <csp/file.h>
#include <csp/blocking.h>
#include <csp/csp.h>

#include <cerrno>
#include <cstring>
#include <fstream>

namespace csp::file {

std::vector<uint8_t> read(const std::string& path) {
    return csp::blocking([&]() -> std::vector<uint8_t> {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            throw csp::error("file::read: " + path + ": " +
                             std::strerror(errno));
        }
        auto size = f.tellg();
        f.seekg(0);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!f.read(reinterpret_cast<char*>(data.data()),
                    static_cast<std::streamsize>(size))) {
            throw csp::error("file::read: " + path + ": read failed");
        }
        return data;
    });
}

void write(const std::string& path, const std::vector<uint8_t>& data) {
    write(path, data.data(), data.size());
}

void write(const std::string& path, const void* data, size_t len) {
    csp::blocking([&] {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) {
            throw csp::error("file::write: " + path + ": " +
                             std::strerror(errno));
        }
        if (!f.write(static_cast<const char*>(data),
                     static_cast<std::streamsize>(len))) {
            throw csp::error("file::write: " + path + ": write failed");
        }
    });
}

} // namespace csp::file
