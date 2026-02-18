#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Pause/resume a stream via a control channel. Starts open. When control
// sends false, the data source backpressures naturally (synchronous channels).
// When control sends true, forwarding resumes. If control dies, the gate
// stays in its last state (open: keep forwarding; closed: close output).
template <typename T>
reader<T> gate(reader<T> data, reader<bool> control) {
    chan<T> out_ch;
    reader<T> result = std::move(out_ch.r);

    csp::spawn([data = std::move(data), control = std::move(control),
                out = std::move(out_ch.w)]() mutable {
        internal::descr("gate");

        T t;
        bool b;
        bool open = true;

        for (;;) {
            if (open) {
                switch (csp::alt(data >> t, control >> b, ~out)) {
                case 1:  // Data — forward.
                    if (!(out << std::move(t))) return;
                    break;
                case 2:  // Control update.
                    open = b;
                    break;
                case ~1:  // Data died.
                    return;
                case ~2:  // Control died while open — keep forwarding.
                    for (T v; csp::alt(data >> v, ~out) > 0;) {
                        if (!(out << std::move(v))) return;
                    }
                    return;
                default:  // Output died.
                    return;
                }
            } else {
                switch (csp::alt(control >> b, ~out)) {
                case 1:  // Control update.
                    open = b;
                    break;
                case ~1:  // Control died while closed — permanently closed.
                    return;
                default:  // Output died.
                    return;
                }
            }
        }
    });

    return result;
}

}
