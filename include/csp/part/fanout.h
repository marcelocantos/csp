#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Broadcast each value to a dynamic set of subscribers.
// New subscribers register by sending their writer<T> endpoint via a
// control channel. Dead subscribers are automatically removed.
template <typename T>
inline auto const fanout = make_filter<writer<T>>([](reader<writer<T>> new_out, writer<writer<T>> new_in) {
    internal::descr("fanout");

    static Logger scope("chan/fanout/scope");
    BRAC_SCOPE(scope, "fanout", "");

    static Logger log("chan/fanout/log");

    for (writer<T> out; prialt(~new_in, new_out >> out) >= 0;) {
        CSP_LOG(log, "first new_out");

        reader<T> in;
        writer<T> in_val = ++in;  // slot 0 write buffer
        T t{};                     // slot 1 read buffer
        writer<T> out_val;         // slot 2 read buffer

        // Slot layout (0-based prialt results):
        //   0: new_in << in_val   (writer<T> transfer)
        //   1: in >> t             (T transfer) — initially empty
        //   2: new_out >> out_val  (writer<T> transfer)
        //   3+: ~outs[i]           (death watches, no transfer)
        std::vector<internal::ChanOp> chanops;
        chanops.push_back({internal::wait(new_in.internal_writer()), &in_val});
        chanops.push_back({{}, nullptr});
        chanops.push_back({internal::wait(new_out.internal_reader()), &out_val});
        chanops.push_back({internal::wait_dead(out.internal_writer()), nullptr});

        std::vector<writer<T>> outs;
        outs.push_back(std::move(out));

        auto drop = [&](size_t i) {
            outs[i] = std::move(outs.back());
            outs.pop_back();
            chanops[3 + i] = chanops.back();
            chanops.pop_back();
        };

        while (!outs.empty()) {
            internal::AltMatch m;
            internal::prialt_begin(&m, chanops.data(), chanops.size(), 0);
            if (m.src && m.dst) {
                int idx = (m.result >= 0 ? m.result : ~m.result);
                if (idx == 0 || idx == 2)
                    *static_cast<writer<T>*>(m.dst) = std::move(*static_cast<writer<T>*>(m.src));
                else if (idx == 1)
                    *static_cast<T*>(m.dst) = std::move(*static_cast<T*>(m.src));
            }
            internal::alt_end(&m);

            switch (m.result) {
            case 0:
                CSP_LOG(log, "new_in");
                chanops[0] = {{}, nullptr};
                chanops[1] = {internal::wait(in.internal_reader()), &t};
                break;
            case ~0:
                CSP_LOG(log, "~new_in");
                return;
            case 1:
                CSP_LOG(log, "in");
                // Traverse backwards in case of in-situ deletions.
                for (auto oi = end(outs); oi-- != begin(outs);) {
                    CSP_LOG(log, "out << t");
                    if (!(*oi << t)) {
                        CSP_LOG(log, "~out");
                        drop(oi - begin(outs));
                    }
                }
                break;
            case ~1:
                CSP_LOG(log, "~in");
                in = {};
                in_val = ++in;
                chanops[0] = {internal::wait(new_in.internal_writer()), &in_val};
                chanops[1] = {{}, nullptr};
                break;
            case 2:  // new_out
                CSP_LOG(log, "new_out");
                chanops.push_back({internal::wait_dead(out_val.internal_writer()), nullptr});
                outs.push_back(std::move(out_val));
                break;
            case ~2:
                CSP_LOG(log, "~new_out");
                // No more new outs.
                chanops[2] = {{}, nullptr};
                break;
            default: {  // ~outs
                auto i = ~m.result - 3;
                CSP_LOG(log, "~outs[%d]", i);
                drop(i);
            }
            }
        }
    }
});

}
