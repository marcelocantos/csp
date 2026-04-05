#pragma once

#ifdef CSP_TLS

#include <csp/csp.h>
#include <csp/io.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace csp::tls {

struct error : csp::error {
    int code;
    error(int code);
};

class conn;

/// Callback for custom certificate verification.
/// Receives the server name and DER-encoded certificate chain.
/// Return true to accept, false to reject.
using verify_fn =
    std::function<bool(const char* server_name,
                       const std::vector<std::vector<uint8_t>>& certs)>;

class context {
public:
    enum role { client, server };
    explicit context(role r = client);
    ~context();
    context(context&&) noexcept;
    context& operator=(context&&) noexcept;

    /// Load certificate chain from a PEM file.
    void load_cert(const char* cert_pem_path);

    /// Load private key from a PEM file (PKCS#8 format, secp256r1).
    void load_key(const char* key_pem_path);

    /// Set a custom certificate verification callback. Without this,
    /// no certificate verification is performed (TLS 1.3 only,
    /// minicrypto backend has no built-in X.509 chain validator).
    void set_verify(verify_fn fn);

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
    conn(context& ctx, io::fd_t fd);
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
    io::fd_t fd() const;

    conn(const conn&) = delete;
    conn& operator=(const conn&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace csp::tls

#endif // CSP_TLS
