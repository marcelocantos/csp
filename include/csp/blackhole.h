#pragma once

#include <csp/csp.h>

namespace csp {

        template <typename T>
        auto blackhole(reader<T> in) {
            return [in = std::move(in)]{
                for (T _; in >> _;) { }
            };
        }

        template <typename T>
        writer<T> spawn_blackhole() {
            return spawn_consumer<T>([](auto && r) mutable {
                blackhole(std::move(r))();
            });
        }

}
