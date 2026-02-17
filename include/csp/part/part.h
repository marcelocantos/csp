#pragma once

#include <csp/csp.h>

#include <type_traits>
#include <utility>

namespace csp::part {

    template <typename T, typename F>
    struct consumer {
        F body_;

        void operator()(reader<T> r) { body_(std::move(r)); }

        auto bind(reader<T> r) const & {
            return [b = body_, r = std::move(r)]() mutable {
                b(std::move(r));
            };
        }
        auto bind(reader<T> r) && {
            return [b = std::move(body_), r = std::move(r)]() mutable {
                b(std::move(r));
            };
        }

        writer<T> spawn() const & {
            return spawn_consumer<T>([b = body_](reader<T> r) mutable {
                b(std::move(r));
            });
        }
        writer<T> spawn() && {
            return spawn_consumer<T>([b = std::move(body_)](reader<T> r) mutable {
                b(std::move(r));
            });
        }
    };

    template <typename T, typename F>
    struct producer {
        F body_;

        void operator()(writer<T> w) { body_(std::move(w)); }

        auto bind(writer<T> w) const & {
            return [b = body_, w = std::move(w)]() mutable {
                b(std::move(w));
            };
        }
        auto bind(writer<T> w) && {
            return [b = std::move(body_), w = std::move(w)]() mutable {
                b(std::move(w));
            };
        }

        reader<T> spawn() const & {
            return spawn_producer<T>([b = body_](writer<T> w) mutable {
                b(std::move(w));
            });
        }
        reader<T> spawn() && {
            return spawn_producer<T>([b = std::move(body_)](writer<T> w) mutable {
                b(std::move(w));
            });
        }
    };

    template <typename In, typename Out, typename F>
    struct filter {
        F body_;

        void operator()(reader<In> r, writer<Out> w) {
            body_(std::move(r), std::move(w));
        }

        auto bind(reader<In> r, writer<Out> w) const & {
            return [b = body_, r = std::move(r), w = std::move(w)]() mutable {
                b(std::move(r), std::move(w));
            };
        }
        auto bind(reader<In> r, writer<Out> w) && {
            return [b = std::move(body_), r = std::move(r), w = std::move(w)]() mutable {
                b(std::move(r), std::move(w));
            };
        }

        writer<In> spawn(writer<Out> w) const & {
            return spawn_consumer<In>(
                [b = body_, w = std::move(w)](reader<In> r) mutable {
                    b(std::move(r), std::move(w));
                });
        }
        writer<In> spawn(writer<Out> w) && {
            return spawn_consumer<In>(
                [b = std::move(body_), w = std::move(w)](reader<In> r) mutable {
                    b(std::move(r), std::move(w));
                });
        }

        reader<Out> spawn(reader<In> r) const & {
            return spawn_producer<Out>(
                [b = body_, r = std::move(r)](writer<Out> w) mutable {
                    b(std::move(r), std::move(w));
                });
        }
        reader<Out> spawn(reader<In> r) && {
            return spawn_producer<Out>(
                [b = std::move(body_), r = std::move(r)](writer<Out> w) mutable {
                    b(std::move(r), std::move(w));
                });
        }

        template <typename T = In, std::enable_if_t<std::is_same_v<T, Out>, int> = 0>
        chan<T> spawn() const & {
            return spawn_filter<T>(
                [b = body_](reader<T> r, writer<T> w) mutable {
                    b(std::move(r), std::move(w));
                });
        }
        template <typename T = In, std::enable_if_t<std::is_same_v<T, Out>, int> = 0>
        chan<T> spawn() && {
            return spawn_filter<T>(
                [b = std::move(body_)](reader<T> r, writer<T> w) mutable {
                    b(std::move(r), std::move(w));
                });
        }
    };

    template <typename T, typename F>
    consumer<T, std::decay_t<F>> make_consumer(F&& f) {
        return {std::forward<F>(f)};
    }

    template <typename T, typename F>
    producer<T, std::decay_t<F>> make_producer(F&& f) {
        return {std::forward<F>(f)};
    }

    template <typename In, typename Out = In, typename F>
    filter<In, Out, std::decay_t<F>> make_filter(F&& f) {
        return {std::forward<F>(f)};
    }

}
