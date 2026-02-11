#include <csp/internal/microthread_internal.h>

#include <iostream>
#include <stdexcept>

namespace csp {

    writer<std::exception_ptr> global_exception_handler = ++channel<std::exception_ptr>{};

    poke_t poke;

    reader<> const skip = --channel<>();

    namespace detail {

        static Microthread main_;
        Microthread * g_self = &main_;
        Microthread * g_busy = &main_;

    }

}
