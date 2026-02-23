#pragma once

#include <cstdlib>
#include <memory>
#include <utility>

namespace csp {

template <typename F>
class OnScopeExit {
public:
    OnScopeExit(F f) : f_(std::move(f)) { }
    ~OnScopeExit() { f_(); }

private:
    F f_;
};

// Assign the return value to a local variable.
template <typename F>
OnScopeExit<F> onScopeExit(F f) {
    return {std::move(f)};
};

inline auto onScopeExitFree(void * p) {
    return onScopeExit([p]{
        std::free(p);
    });
}

template <typename T, typename F>
class ScopedResource {
public:
    ScopedResource(T t, F f) : t_(std::move(t)), f_(std::make_unique<F>(std::move(f))) { }
    ScopedResource(ScopedResource &&) = default;
    ~ScopedResource() {
        if (f_) {
            (*f_)(t_);
        }
    }

    T const * operator->() const { return &t_; }
    T const & operator*() const { return t_; }

private:
    T t_;
    std::unique_ptr<F> f_;
};

template <typename T, typename F>
auto scopedResource(T t, F f) {
    return ScopedResource<T, F>(t, std::move(f));
}

template <typename T>
auto mallocedResource(T * p) {
    return scopedResource(p, [](void * p){ free(p); });
}

}
