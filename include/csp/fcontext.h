#pragma once

#include <cstddef>

// Types and functions matching vendored Boost.Context assembly
// (third_party/boost-context/src/asm/).  The assembly uses C linkage,
// so we declare extern "C" to suppress name mangling.

namespace csp {

using fcontext_t = void *;

struct transfer_t {
    fcontext_t fctx;
    void * data;
};

extern "C" {
transfer_t jump_fcontext(fcontext_t to, void * vp);
fcontext_t make_fcontext(void * sp, std::size_t size, void (* fn)(transfer_t));
}

}
