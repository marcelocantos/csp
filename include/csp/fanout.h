#pragma once

#include <csp/microthread.h>

namespace csp {

    // Transfer incoming messages from one reader to multiple
    // writers.
    //
    // If there are no writers, incoming messages are dropped.
    // This is impossible to avoid, since writers may go away
    // after a message is read.
    //
    // new_out: Reads new output channels to deliver messages to.
    //
    // new_in: Writes input channels when there are active output
    //         channels. Input channels die when output count
    //         reaches zero.
    //
    // The inner loop uses a raw csp_chanop vector because the
    // operations span two channel types (channel<writer<T>> for
    // the control plane, channel<T> for the data plane).  The
    // transfer is dispatched inline by slot index — no function
    // pointer, no BLR — because each slot's type is fixed at
    // compile time.
    template <typename T>
    auto fanout(reader<writer<T>> new_out, writer<writer<T>> new_in) {
        return [new_out = std::move(new_out), new_in = std::move(new_in)]{
            csp_descr("fanout");

            static Logger scope("chan/fanout/scope");
            BRAC_SCOPE(scope, "fanout", "");

            static Logger log("chan/fanout/log");

            for (writer<T> out; prialt(~new_in, new_out >> out) > 0;) {
                CSP_LOG(log, "first new_out");

                reader<T> in;
                writer<T> in_val = ++in;  // slot 0 write buffer
                T t{};                     // slot 1 read buffer
                writer<T> out_val;         // slot 2 read buffer

                // Slot layout (indices are 1-based in prialt results):
                //   0: new_in << in_val   (writer<T> transfer)
                //   1: in >> t             (T transfer) — initially empty
                //   2: new_out >> out_val  (writer<T> transfer)
                //   3+: ~outs[i]           (death watches, no transfer)
                std::vector<csp_chanop> chanops;
                chanops.push_back({csp_wait(new_in.internal_writer()), &in_val});
                chanops.push_back({nullptr, nullptr});
                chanops.push_back({csp_wait(new_out.internal_reader()), &out_val});
                chanops.push_back({csp_wait_dead(out.internal_writer()), nullptr});

                std::vector<writer<T>> outs;
                outs.push_back(std::move(out));

                auto drop = [&](size_t i) {
                    outs[i] = std::move(outs.back());
                    outs.pop_back();
                    chanops[3 + i] = chanops.back();
                    chanops.pop_back();
                };

                while (!outs.empty()) {
                    csp_alt_match m;
                    csp_prialt_begin(&m, chanops.data(), chanops.size(), 0);
                    if (m.src && m.dst) {
                        int idx = (m.result > 0 ? m.result : -m.result) - 1;
                        if (idx == 0 || idx == 2)
                            *static_cast<writer<T>*>(m.dst) = std::move(*static_cast<writer<T>*>(m.src));
                        else if (idx == 1)
                            *static_cast<T*>(m.dst) = std::move(*static_cast<T*>(m.src));
                    }
                    csp_alt_end(&m);

                    switch (m.result) {
                    case 1:
                        CSP_LOG(log, "new_in");
                        chanops[0] = {nullptr, nullptr};
                        chanops[1] = {csp_wait(in.internal_reader()), &t};
                        break;
                    case -1:
                        CSP_LOG(log, "~new_in");
                        return;
                    case 2:
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
                    case -2:
                        CSP_LOG(log, "~in");
                        in = {};
                        in_val = ++in;
                        chanops[0] = {csp_wait(new_in.internal_writer()), &in_val};
                        chanops[1] = {nullptr, nullptr};
                        break;
                    case 3:  // new_out
                        CSP_LOG(log, "new_out");
                        chanops.push_back({csp_wait_dead(out_val.internal_writer()), nullptr});
                        outs.push_back(std::move(out_val));
                        break;
                    case -3:
                        CSP_LOG(log, "~new_out");
                        // No more new outs.
                        chanops[2] = {nullptr, nullptr};
                        break;
                    default: {  // ~outs
                        auto i = -4 - m.result;
                        CSP_LOG(log, "~outs[%d]", i);
                        drop(i);
                    }
                    }
                }
            }
        };
    }

    template <typename T>
    writer<writer<T>> spawn_fanout(writer<writer<T>> new_in) {
        return spawn_consumer<writer<T>>([new_in = std::move(new_in)](auto new_out) mutable {
            fanout(std::move(new_out), std::move(new_in))();
        });
    }

    template <typename T>
    reader<writer<T>> spawn_fanout(reader<writer<T>> new_out) {
        return spawn_producer<writer<T>>([new_out = std::move(new_out)](auto new_in) mutable {
            fanout(std::move(new_out), std::move(new_in))();
        });
    }

}
