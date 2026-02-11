#ifndef INCLUDED__csp__quantize_h
#define INCLUDED__csp__quantize_h

#include <csp/microthread.h>
#include <csp/internal/on_scope_exit.h>

#include <sstream>

namespace csp {

    namespace chan {

        template <typename T>
        auto quantize(reader<T> source,  // incoming units
                      reader<T> quanta,  // quanta to emit
                      writer<T> sink,    // outgoing quanta
                      writer<T> residue = ++channel<T>{}) // after all qanta delivered
        {
            return [=]{
                csp_descr("chan::quantize");

                static Logger log("chan/quantize");

                T acc = {}, q = {}, t = {};

                auto deliver_residue = onScopeExit([&]{
                    CSP_LOG(log, "quantize: residue << %d", t);
                    residue << acc;
                });

                for (;;) {
                    switch (int rc = alt(acc < q ? source >> t : ~source,
                                         !q ? quanta >> q : ~quanta,
                                         q && q <= acc ? sink << q : ~sink)) {
                    case 1: // source
                        CSP_LOG(log, "quantize: source >> %d", t);
                        acc += t;
                        break;
                    case 2: // quanta
                        CSP_LOG(log, "quantize: quanta >> %d", q);
                        // Deliver 0-quantum immediately.
                        if (!q && !(sink << q)) {
                            return;
                        }
                        break;
                    case 3: // sink
                        CSP_LOG(log, "quantize: sink << %d", q);
                        acc -= q;
                        q = 0;
                        break;
                    default:
                        CSP_LOG(log, "quantize: -%d", -rc);
                        if (rc == -1) { // Dead source; deliver quantum, if any.
                            if (q && q <= acc && sink << q) {
                                CSP_LOG(log, "quantize[~source]: sink << %d", q);
                                acc -= q;
                            }
                        } else if (rc == -2) { // Dead quanta; drain source.
                            while (q) {
                                switch (int rc = alt(acc < q ? source >> t : ~source,
                                                     q <= acc ? sink << q : ~sink)) {
                                case 1: // source
                                    CSP_LOG(log, "quantize[~quanta]: source >> %d", t);
                                    acc += t;
                                    break;
                                case 2: // sink
                                    CSP_LOG(log, "quantize[~quanta]: sink << %d", q);
                                    acc -= q;
                                    return;
                                default:
                                    CSP_LOG(log, "quantize[~quanta]: ~%d", -rc);
                                    if (q && q <= acc && sink << q) {
                                        CSP_LOG(log, "quantize[~quanta,~%s]: sink << %d",
                                                 rc == -1 ? "source" : "sink", q);
                                        acc -= q;
                                    }
                                    return;
                                }
                            }
                        }
                        return;
                    }
                }
            };
        }

        template <typename T>
        writer<T> spawn_quantize(reader<T> quanta, writer<T> sink, writer<T> residue = ++channel<T>{}) {
            return spawn_consumer<T>([=](auto source) {
                quantize(source, quanta, sink, residue);
            });
        }

        template <typename T>
        auto quantize(reader<T> source, // incoming units
                      T quantum,        // quanta to emit
                      writer<T> sink,    // outgoing quanta
                      writer<T> residue = ++channel<T>{})
        {
            return [=]{
                T acc = 0, t = {};
                for (;;) {
                    switch (alt(acc < quantum ? source >> t : ~source,
                                quantum <= acc ? sink << quantum : ~sink))
                    {
                    case 1:  // source
                        acc += t;
                        break;
                    case 2:  // sink
                        acc -= quantum;
                        break;
                    default:
                        residue << acc;
                        return;
                    }
                }
            };
        }

        template <typename T>
        reader<T> spawn_quantize(reader<T> source, T quantum, writer<T> residue = ++channel<T>{}) {
            return spawn_producer<T>([=](auto sink) {
                quantize(source, quantum, sink, residue)();
            });
        }

        template <typename T>
        writer<double> spawn_quantize(T quantum, writer<T> sink, writer<T> residue = ++channel<T>{}) {
            return spawn_consumer<T>([=](auto source) {
                quantize(source, quantum, sink, residue)();
            });
        }

    }

}

#endif // INCLUDED__csp__quantize_h
