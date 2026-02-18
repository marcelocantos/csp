#ifndef SIMPLE_STACK_ALLOCATOR_H
#define SIMPLE_STACK_ALLOCATOR_H

#include <cassert>
#include <cstddef>

namespace boost {
namespace context {

template <std::size_t Max, std::size_t Default, std::size_t Min>
class simple_stack_allocator {
public:
    static constexpr std::size_t max_stack = Max;
    static constexpr std::size_t def_stack = Default;
    static constexpr std::size_t min_stack = Min;

    void * allocate(std::size_t size = def_stack) const {
        assert(min_stack <= size);
        assert(size <= max_stack);

        return new char[size] + size;
    }

    void deallocate(void * vp, std::size_t size = def_stack) const {
        assert(vp);
        assert(min_stack <= size);
        assert(size <= max_stack);

        delete [] (static_cast<char *>(vp) - size);
    }
};

}}

#endif // SIMPLE_STACK_ALLOCATOR_H
