#pragma once

#include <csp/csp.h>
#include "ringbuffer.h"

#include <cassert>
#include <algorithm>

namespace csp {

        template <typename T>
        auto buffer(reader<T> input, writer<T> output, size_t capacity = size_t(-1)) {
            return [in = std::move(input), out = std::move(output), capacity] {
                csp_descr("buffer");

                static Logger log("chan/buffer");
                static Logger scope("chan/buffer/scope");
                BRAC_SCOPE(scope, "buffer", "%lu", capacity);

                detail::RingBuffer<T> buf(capacity);
                for (;;) {
                    CSP_LOG(log, "buffer state: %s", buf.empty() ? "EMPTY" : buf.full() ? "FULL" : "JUST RIGHT");
                    switch (auto slot = alt(buf.full()  ? ~in  : in  >> buf.next(),
                                            buf.empty() ? ~out : out << buf.front())) {
                    case 1:
                        CSP_LOG(log, "IN");
                        // Input ready
                        buf.push();
                        CSP_LOG(log, "PUSH%s", buf.full() ? " (full)" : "");
                        break;
                    case -1:
                        CSP_LOG(log, "DRAIN");
                        // No more input; drain the buffer and go away.
                        while (!buf.empty() && out << buf.front()) {
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
            };
        }

        //----------------------------------------------------------------
        // spawn_* overloads

        // Wire up an existing downstream writer, returning an upstream writer.
        template <typename T>
        writer<T> spawn_buffer(writer<T> w, size_t capacity = size_t(-1)) {
            return spawn_consumer<T>([w = std::move(w), capacity](auto && r) mutable {
                buffer(std::move(r), std::move(w), capacity)();
            });
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename T>
        reader<T> spawn_buffer(reader<T> r, size_t capacity = size_t(-1)) {
            return spawn_producer<T>([r = std::move(r), capacity](auto && w) mutable {
                buffer(std::move(r), std::move(w), capacity)();
            });
        }

        template <typename T>
        chan<T> spawn_buffer(size_t capacity = size_t(-1)) {
            return spawn_filter<T>([capacity](auto && r, auto && w) mutable {
                buffer(std::move(r), std::move(w), capacity)();
            });
        }

}
