#pragma once

#include <csp/part/part.h>
#include <csp/ringbuffer.h>

#include <cassert>
#include <algorithm>

namespace csp::part {

// Bounded (or unbounded) FIFO buffer between producer and consumer.
// Decouples production rate from consumption rate up to the given capacity.
template <typename T>
auto buffer(size_t capacity = size_t(-1)) {
    return make_filter<T>([capacity](reader<T> in, writer<T> out) {
        internal::descr("buffer");

        static Logger log("chan/buffer");
        static Logger scope("chan/buffer/scope");
        BRAC_SCOPE(scope, "buffer", "%lu", capacity);

        csp::detail::RingBuffer<T> buf(capacity);
        for (;;) {
            CSP_LOG(log, "buffer state: %s", buf.empty() ? "EMPTY" : buf.full() ? "FULL" : "JUST RIGHT");
            switch (auto slot = alt(buf.full()  ? ~in  : in  >> buf.next(),
                                    buf.empty() ? ~out : out << buf.front())) {
            case 1:
                CSP_LOG(log, "IN");
                buf.push();
                CSP_LOG(log, "PUSH%s", buf.full() ? " (full)" : "");
                break;
            case -1:
                CSP_LOG(log, "DRAIN");
                while (!buf.empty() && out << std::move(buf.front())) {
                    buf.pop();
                }
                return;
            case 2:
                CSP_LOG(log, "OUT");
                buf.pop();
                CSP_LOG(log, "POP %s", buf.empty() ? " (empty)": "");
                break;
            case -2:
                CSP_LOG(log, "~OUT");
                return;
            default:
                assert(!"Uh?");
            }
        }
    });
}

}
