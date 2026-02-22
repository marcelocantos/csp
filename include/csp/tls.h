#pragma once

#ifdef CSP_TLS

#include <csp/csp.h>

#include <memory>
#include <string>

namespace csp::tls {

struct error : csp::error {
    int code;
    error(int code);
};

class conn;

class context {
public:
    enum role { client, server };
    explicit context(role r = client);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;

    // Load CA certificate(s) for verification. PEM, NUL-terminated.
    void load_ca(const void* pem, size_t len);

    // Load own certificate + private key (server required, client optional).
    void load_cert(const void* cert_pem, size_t cert_len,
                   const void* key_pem, size_t key_len);

    context(const context&) = delete;
    context& operator=(const context&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    friend class conn;
};

class conn {
public:
    // fd must be connected and non-blocking.
    conn(context& ctx, int fd);
    ~conn();
    conn(conn&&) noexcept;
    conn& operator=(conn&&) noexcept;

    // Set hostname for SNI and certificate verification.
    void set_hostname(const std::string& hostname);

    // Perform TLS handshake. Cancel-aware.
    void handshake();

    // Read up to len bytes. Returns bytes read, 0 on clean shutdown.
    ssize_t read(void* buf, size_t len);

    // Write all bytes. Returns total bytes written.
    ssize_t write(const void* buf, size_t len);

    // Send close_notify and shut down TLS session.
    void shutdown();

    // Return the underlying fd.
    int fd() const;

    conn(const conn&) = delete;
    conn& operator=(const conn&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace csp::tls

#endif // CSP_TLS
