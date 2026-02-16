#pragma once

#include <csp/microthread.h>

namespace csp {

        template <typename T>
        auto mute(writer<T> out) {
            return [out = std::move(out)]{
                csp_descr("mute");

                alt(~out);
            };
        }

        template <typename T = poke_t>
        reader<T> spawn_mute() {
            return spawn_producer<T>([](auto && r) mutable {
                mute(std::move(r))();
            });
        }

}
