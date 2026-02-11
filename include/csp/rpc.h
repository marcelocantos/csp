#ifndef INCLUDED__csp__rpc_h
#define INCLUDED__csp__rpc_h

#include <csp/microthread.h>

#include <csp/internal/function.h>

#include <tuple>
#include <utility>

namespace csp {

    namespace chan {

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

        }
        // rpc using channel-pair.  Server must deliver each reply
        // before accepting further requests.

        template <typename... Args, typename Rep>
        auto rpc_client(writer<std::tuple<Args...>> req, reader<Rep> rep) {
            // TODO: perfect forwarding
            return [req = std::move(req), rep = std::move(rep)](Args... args) {
                if (alt(req << std::make_tuple(std::forward<Args>(args)...), ~rep) == 1) {
                    return rep.read();
                }
                throw std::runtime_error("rpc dead");
            };
        };

        template <typename... Args, typename Rep, typename F>
        auto rpc_server(reader<std::tuple<Args...>> req, writer<Rep> rep, F && f) {
            return [req = std::move(req), rep = std::move(rep), f = std::move(f)]{
                std::tuple<Args...> t;
                while (alt(req >> t, ~rep) == 1) {
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
                channel<Rep> rep;
                if (req << std::make_pair(std::forward<std::decay_t<decltype(t)>>(t), +rep)) {
                    return (-rep).read();
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

}

#endif // INCLUDED__csp__rpc_h
