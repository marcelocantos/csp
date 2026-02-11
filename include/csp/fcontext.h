#ifndef INCLUDED__csp__fcontext_h
#define INCLUDED__csp__fcontext_h

#include <boost/context/detail/fcontext.hpp>

namespace csp {

    using fcontext_t = boost::context::detail::fcontext_t;
    using transfer_t = boost::context::detail::transfer_t;

    using boost::context::detail::jump_fcontext;
    using boost::context::detail::make_fcontext;

}

#endif // INCLUDED__csp__fcontext_h
