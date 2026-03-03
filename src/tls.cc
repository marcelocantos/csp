#ifdef CSP_TLS

#include <csp/tls.h>
#include <csp/io.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace csp::tls {

// --- error ---

static std::string format_error(int code) {
    char buf[256];
    mbedtls_strerror(code, buf, sizeof(buf));
    return std::string("tls: ") + buf;
}

error::error(int code) : csp::error(format_error(code)), code(code) {}

// --- BIO callbacks (non-blocking, never throw) ---

static int csp_tls_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    ssize_t n = ::read(fd, buf, len);
    if (n > 0) return static_cast<int>(n);
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

static int csp_tls_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    ssize_t n = ::write(fd, buf, len);
    if (n >= 0) return static_cast<int>(n);
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

// --- Retry helper ---
// Loops mbedTLS operations, calling wait_readable/wait_writable on
// WANT_READ/WANT_WRITE. Both cases checked in every call to handle
// TLS renegotiation (read needing write and vice versa).

template <typename Fn>
static int retry_tls_io(int fd, Fn&& fn) {
    for (;;) {
        int ret = fn();
        if (ret >= 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ) {
            csp::io::wait_readable(fd);
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            csp::io::wait_writable(fd);
            continue;
        }
        return ret;
    }
}

// --- context ---

struct context::impl {
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt ca_chain;
    mbedtls_x509_crt own_cert;
    mbedtls_pk_context own_key;
};

context::context(role r) : impl_(std::make_unique<impl>()) {
    mbedtls_ssl_config_init(&impl_->conf);
    mbedtls_entropy_init(&impl_->entropy);
    mbedtls_ctr_drbg_init(&impl_->ctr_drbg);
    mbedtls_x509_crt_init(&impl_->ca_chain);
    mbedtls_x509_crt_init(&impl_->own_cert);
    mbedtls_pk_init(&impl_->own_key);

    int ret = mbedtls_ctr_drbg_seed(&impl_->ctr_drbg,
                                     mbedtls_entropy_func,
                                     &impl_->entropy,
                                     nullptr, 0);
    if (ret != 0) throw error(ret);

    int endpoint = (r == server) ? MBEDTLS_SSL_IS_SERVER
                                 : MBEDTLS_SSL_IS_CLIENT;
    ret = mbedtls_ssl_config_defaults(&impl_->conf,
                                       endpoint,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) throw error(ret);

    mbedtls_ssl_conf_rng(&impl_->conf,
                          mbedtls_ctr_drbg_random,
                          &impl_->ctr_drbg);

    // Default: verify peer for clients, none for servers.
    mbedtls_ssl_conf_authmode(&impl_->conf,
        (r == client) ? MBEDTLS_SSL_VERIFY_REQUIRED
                      : MBEDTLS_SSL_VERIFY_NONE);
}

context::~context() {
    if (!impl_) return;
    mbedtls_pk_free(&impl_->own_key);
    mbedtls_x509_crt_free(&impl_->own_cert);
    mbedtls_x509_crt_free(&impl_->ca_chain);
    mbedtls_ctr_drbg_free(&impl_->ctr_drbg);
    mbedtls_entropy_free(&impl_->entropy);
    mbedtls_ssl_config_free(&impl_->conf);
}

context::context(context&&) noexcept = default;
context& context::operator=(context&&) noexcept = default;

void context::load_ca(const void* pem, size_t len) {
    int ret = mbedtls_x509_crt_parse(&impl_->ca_chain,
                                      static_cast<const unsigned char*>(pem),
                                      len);
    if (ret != 0) throw error(ret);
    mbedtls_ssl_conf_ca_chain(&impl_->conf, &impl_->ca_chain, nullptr);
}

void context::load_cert(const void* cert_pem, size_t cert_len,
                        const void* key_pem, size_t key_len) {
    int ret = mbedtls_x509_crt_parse(&impl_->own_cert,
                                      static_cast<const unsigned char*>(cert_pem),
                                      cert_len);
    if (ret != 0) throw error(ret);

    ret = mbedtls_pk_parse_key(&impl_->own_key,
                                static_cast<const unsigned char*>(key_pem),
                                key_len,
                                nullptr, 0,
                                mbedtls_ctr_drbg_random,
                                &impl_->ctr_drbg);
    if (ret != 0) throw error(ret);

    ret = mbedtls_ssl_conf_own_cert(&impl_->conf,
                                     &impl_->own_cert,
                                     &impl_->own_key);
    if (ret != 0) throw error(ret);
}

// --- conn ---

struct conn::impl {
    mbedtls_ssl_context ssl;
    int fd;
};

conn::conn(context& ctx, int fd) : impl_(std::make_unique<impl>()) {
    impl_->fd = fd;

    // Suppress SIGPIPE on this fd — TLS connections handle write
    // errors via return codes, not signals.
#ifdef F_SETNOSIGPIPE
    fcntl(fd, F_SETNOSIGPIPE, 1);
#endif

    mbedtls_ssl_init(&impl_->ssl);

    int ret = mbedtls_ssl_setup(&impl_->ssl, &ctx.impl_->conf);
    if (ret != 0) {
        mbedtls_ssl_free(&impl_->ssl);
        throw error(ret);
    }

    mbedtls_ssl_set_bio(&impl_->ssl,
                         &impl_->fd,
                         csp_tls_send,
                         csp_tls_recv,
                         nullptr);
}

conn::~conn() {
    if (impl_) {
        mbedtls_ssl_free(&impl_->ssl);
    }
}

conn::conn(conn&&) noexcept = default;
conn& conn::operator=(conn&&) noexcept = default;

void conn::set_hostname(const std::string& hostname) {
    int ret = mbedtls_ssl_set_hostname(&impl_->ssl, hostname.c_str());
    if (ret != 0) throw error(ret);
}

void conn::handshake() {
    int ret = retry_tls_io(impl_->fd, [&] {
        return mbedtls_ssl_handshake(&impl_->ssl);
    });
    if (ret != 0) throw error(ret);
}

ssize_t conn::read(void* buf, size_t len) {
    int ret = retry_tls_io(impl_->fd, [&] {
        return mbedtls_ssl_read(&impl_->ssl,
                                 static_cast<unsigned char*>(buf),
                                 len);
    });
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if (ret < 0) throw error(ret);
    return ret;
}

ssize_t conn::write(const void* buf, size_t len) {
    auto p = static_cast<const unsigned char*>(buf);
    size_t written = 0;
    while (written < len) {
        int ret = retry_tls_io(impl_->fd, [&] {
            return mbedtls_ssl_write(&impl_->ssl,
                                      p + written,
                                      len - written);
        });
        if (ret < 0) throw error(ret);
        written += static_cast<size_t>(ret);
    }
    return static_cast<ssize_t>(written);
}

void conn::shutdown() {
    // Ignore errors on close_notify — peer may have already closed.
    retry_tls_io(impl_->fd, [&] {
        return mbedtls_ssl_close_notify(&impl_->ssl);
    });
}

int conn::fd() const {
    return impl_->fd;
}

} // namespace csp::tls

#endif // CSP_TLS
