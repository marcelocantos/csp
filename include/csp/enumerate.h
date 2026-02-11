#ifndef INCLUDED__csp__enumerate_h
#define INCLUDED__csp__enumerate_h

#include <csp/microthread.h>

#include <vector>

namespace csp {

    namespace chan {

        template <typename T, typename C>
        auto enumerate(C && c, writer<T> sink, bool cyclic = false) {
            csp_descr("chan::enumerate");

            return [c = std::move(c), sink = std::move(sink), cyclic]{
                do {
                    for (auto const & e : c) {
                        if (!(sink << e)) {
                            return;
                        }
                    }
                } while (cyclic);
            };
        }

        template <typename T>
        auto enumerate(std::initializer_list<T> c, writer<T> sink, bool cyclic = false) {
            return enumerate(std::vector<T>(c), sink, cyclic);
        }

        template <typename T, typename C>
        auto cycle(C && c, writer<T> sink) {
            return enumerate(c, sink, true);
        }

        template <typename T>
        auto cycle(std::initializer_list<T> c, writer<T> sink) {
            return enumerate(c, sink, true);
        }

        template <typename T, typename C>
        reader<T> spawn_enumerate(C && c, bool cyclic = false) {
            return spawn_producer<T>([c = std::move(c), cyclic](auto && w) {
                enumerate(c, w, cyclic)();
            });
        }

        template <typename T>
        reader<T> spawn_enumerate(std::initializer_list<T> c, bool cyclic = false) {
            return spawn_enumerate<T>(std::vector<T>(c), cyclic);
        }

        template <typename T, typename C>
        reader<T> spawn_cycle(C && c) {
            return spawn_enumerate<T>(c, true);
        }

        template <typename T>
        reader<T> spawn_cycle(std::initializer_list<T> c) {
            return spawn_enumerate<T>(c, true);
        }

    }

}

#endif // INCLUDED__csp__enumerate_h
