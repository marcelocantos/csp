#pragma once

#include <csp/part/part.h>

#include <exception>

namespace csp::part {

// Transform each element, catching exceptions.
// On success, the mapped value passes through. On exception, the
// exception_ptr is sent to err and the input value is dropped.
template <typename A, typename B = A, typename F>
auto try_map(F&& f, writer<std::exception_ptr> err) {
    return make_filter<A, B>(
        [f = std::forward<F>(f), err = std::move(err)]
        (reader<A> in, writer<B> out) {
            internal::descr("try_map");

            for (A a; alt(in >> a, ~out) == 0;) {
                try {
                    if (!(out << f(a))) break;
                } catch (...) {
                    if (!(err << std::current_exception())) break;
                }
            }
        });
}

// Transform each element (no error channel — exceptions propagate).
template <typename A, typename B = A, typename F>
auto try_map(F&& f) {
    return map<A, B>(std::forward<F>(f));
}

}
