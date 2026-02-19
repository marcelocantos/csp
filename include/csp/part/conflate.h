#pragma once

#include <csp/part/part.h>

#include <utility>

namespace csp::part {

// When downstream is slow, merge pending upstream values via a combining
// function instead of buffering or dropping.  Each value passes through
// individually when the consumer keeps up; when it falls behind, pending
// values are folded with f(accumulated, new_value).
template <typename T, typename F>
auto conflate(F&& f) {
    return make_filter<T>([f = std::forward<F>(f)](reader<T> in, writer<T> out) {
        internal::descr("conflate");

        T pending;
        if (!(in >> pending)) return;

        for (;;) {
            T next;
            switch (csp::alt(out << pending, in >> next)) {
            case 0:  // sent pending downstream
                if (!(in >> pending)) return;
                break;
            case 1:  // got new value while pending unsent
                pending = f(std::move(pending), std::move(next));
                break;
            case ~0: return;  // output died
            case ~1:          // input died — flush last value
                out << std::move(pending);
                return;
            }
        }
    });
}

}
