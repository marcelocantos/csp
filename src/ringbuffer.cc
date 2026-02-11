#include <csp/buffer.h>

#include <stdlib.h>
#include <string.h>

namespace csp {

    namespace detail {

        void * ring_buffer_alloc(size_t n, size_t el_size, size_t el_align) {
            static csp::Logger log("chan/buffer");
            while (el_align < sizeof(void *)) {
                el_align *= 2;
            }
            CSP_LOG(log, "buffer_alloc(%lu, %lu, %lu)", n, el_size, el_align);
            void * p;
            if (int err = posix_memalign(&p, el_align, n * el_size)) {
                throw std::runtime_error(std::string("buffer_alloc failed ") + strerror(err));
            }
            return p;
        }

    }

}
