#ifndef _WIN32

#include <csp/cancel.h>
#include <csp/internal/signal.h>
#include <csp/internal/reactor.h>

namespace csp::internal {

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

} // namespace csp::internal

#endif // !_WIN32
