#include <csp/cancel.h>
#include <csp/internal/signal.h>
#include <csp/internal/reactor.h>

namespace csp::internal {

#ifdef _WIN32

void io_wait_readable(SOCKET sock) {
    auto signal = detail::create_fd_readable(sock);

    if (!csp::is_cancel_active()) {
        csp::prialt(~signal);
        return;
    }

    switch (csp::prialt(csp::done(), ~signal)) {
    case ~0: {
        auto reason = csp::cancel_reason();
        if (reason) std::rethrow_exception(reason);
        throw csp::canceled{};
    }
    case ~1: return;
    }
}

void io_wait_writable(SOCKET sock) {
    auto signal = detail::create_fd_writable(sock);

    if (!csp::is_cancel_active()) {
        csp::prialt(~signal);
        return;
    }

    switch (csp::prialt(csp::done(), ~signal)) {
    case ~0: {
        auto reason = csp::cancel_reason();
        if (reason) std::rethrow_exception(reason);
        throw csp::canceled{};
    }
    case ~1: return;
    }
}

#else // !_WIN32

void io_wait_readable(int fd) {
    auto signal = detail::create_fd_readable(fd);

    if (!csp::is_cancel_active()) {
        csp::prialt(~signal);
        return;
    }

    switch (csp::prialt(csp::done(), ~signal)) {
    case ~0: {
        auto reason = csp::cancel_reason();
        if (reason) std::rethrow_exception(reason);
        throw csp::canceled{};
    }
    case ~1: return;
    }
}

void io_wait_writable(int fd) {
    auto signal = detail::create_fd_writable(fd);

    if (!csp::is_cancel_active()) {
        csp::prialt(~signal);
        return;
    }

    switch (csp::prialt(csp::done(), ~signal)) {
    case ~0: {
        auto reason = csp::cancel_reason();
        if (reason) std::rethrow_exception(reason);
        throw csp::canceled{};
    }
    case ~1: return;
    }
}

#endif // _WIN32

} // namespace csp::internal
