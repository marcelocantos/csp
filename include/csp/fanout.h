#ifndef INCLUDED__csp__fanout_h
#define INCLUDED__csp__fanout_h

#include <csp/microthread.h>

#include <csp/internal/on_scope_exit.h>

namespace csp {

    namespace chan {

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
        template <typename T>
        auto fanout(reader<writer<T>> new_out, writer<writer<T>> new_in) {
            return [=]{
                csp_descr("chan::fanout");

                static Logger scope("chan/fanout/scope");
                BRAC_SCOPE(scope, "fanout", "");

                static Logger log("chan/fanout/log");

                for (writer<T> out; prialt(~new_in, new_out >> out) > 0;) {
                    CSP_LOG(log, "first new_out");

                    reader<T> in;
                    auto actions = action_list(new_in << ++in, action{}, new_out >> out, ~out);

                    std::vector<writer<T>> outs{std::move(out)};

                    auto drop = [&](size_t i) {
                        outs[i] = std::move(outs.back());
                        outs.pop_back();
                        actions[3 + i] = std::move(actions.back());
                        actions.pop_back();
                    };

                    T t;
                    while (!outs.empty()) {
                        switch (auto i = prialt(actions)) {
                        case 1:
                            CSP_LOG(log, "new_in");
                            actions[0] = {};
                            actions[1] = in >> t;
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
                            actions[0] = new_in << ++in;
                            actions[1] = {};
                            break;
                        case 3:  // new_out
                            CSP_LOG(log, "new_out");
                            actions.push_back(~out);
                            outs.push_back(std::move(out));
                            break;
                        case -3:
                            CSP_LOG(log, "~new_out");
                            // No more new outs.
                            actions[2] = {};
                            break;
                        default:  // ~outs
                            i = -4 - i;
                            CSP_LOG(log, "~outs[%d]", i);
                            drop(i);
                        }
                    }
                }
            };
        }

        template <typename T>
        writer<writer<T>> spawn_fanout(writer<writer<T>> new_in) {
            return spawn_consumer<writer<T>>([=](auto new_out) {
                fanout(new_out, new_in)();
            });
        }

        template <typename T>
        reader<writer<T>> spawn_fanout(reader<writer<T>> new_out) {
            return spawn_producer<writer<T>>([=](auto new_in) {
                fanout(new_out, new_in)();
            });
        }

    }

}

#endif // INCLUDED__csp__fanout_h
