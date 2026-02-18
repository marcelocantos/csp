#pragma once

#include <csp/csp.h>

#include <csp/internal/function.h>

#include <tuple>
#include <utility>

namespace csp::part {

namespace detail {

template <typename Ret>
struct apply_message {
    template <typename F, typename Tuple>
    auto operator()(F && f, Tuple && t) {
        return std::apply(std::forward<F>(f), std::forward<Tuple>(t));
    }
};

template <>
struct apply_message<poke_t> {
    template <typename F, typename Tuple>
    auto operator()(F && f, Tuple && t) {
        std::apply(std::forward<F>(f), std::forward<Tuple>(t));
        return poke;
    }
};

} // namespace detail

// rpc using channel-pair.  Server must deliver each reply
// before accepting further requests.

template <typename... Args, typename Rep>
auto rpc_client(writer<std::tuple<Args...>> req, reader<Rep> rep) {
    return [req = std::move(req), rep = std::move(rep)](Args... args) {
        if (alt(req << std::make_tuple(std::move(args)...), ~rep) == 0) {
            return rep.read();
        }
        throw std::runtime_error("rpc dead");
    };
};

template <typename... Args, typename Rep, typename F>
auto rpc_server(reader<std::tuple<Args...>> req, writer<Rep> rep, F && f) {
    return [req = std::move(req), rep = std::move(rep), f = std::move(f)]{
        std::tuple<Args...> t;
        while (alt(req >> t, ~rep) == 0) {
            if (!(rep << detail::apply_message<Rep>{}(f, t))) {
                return;
            }
        }
    };
}

// rpc client includes reply channel in each request.  The server
// is permitted to accept new requests while replies are pending.

template <typename... Args, typename Rep>
auto rpc_client(writer<std::pair<std::tuple<Args...>, writer<Rep>>> req) {
    return [req = std::move(req)](auto && t) {
        auto [w, r] = chan<Rep>{};
        if (req << std::make_pair(std::forward<std::decay_t<decltype(t)>>(t), std::move(w))) {
            return r.read();
        }
        throw std::runtime_error("rpc dead");
    };
}

template <typename... Args, typename Rep, typename F>
auto rpc_server(reader<std::pair<std::tuple<Args...>, writer<Rep>>> req, F && f) {
    return [req = std::move(req), f = std::move(f)]{
        std::pair<std::tuple<Args...>, writer<Rep>> r;
        while (req >> r) {
            r.second << detail::apply_message<Rep>{}(f, r.first);
        }
    };
}

}
