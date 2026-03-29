#pragma once

#include <csp/internal/csp_internal.h>
#include <csp/internal/hamt.h>

#include <any>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace csp {

// --- context_key ---
// Default-constructed instances get a unique ID.
// Copies compare and hash equal.
class context_key {
    uint64_t id_;
public:
    context_key() : id_([]{ static std::atomic<uint64_t> next{0}; return next++; }()) {}
    context_key(const context_key&) = default;
    context_key& operator=(const context_key&) = default;

    uint64_t id() const { return id_; }
    bool operator==(const context_key& o) const { return id_ == o.id_; }
    bool operator!=(const context_key& o) const { return id_ != o.id_; }
};

// --- context ---
// Copyable RAII handle for an HAMT root. Sendable over channels.
class context {
    uintptr_t root_;
public:
    context() : root_(0) {}

    context(const context& o) : root_(o.root_) {
        if (root_) internal::hamt_retain(root_);
    }
    context(context&& o) noexcept : root_(o.root_) { o.root_ = 0; }

    ~context() { if (root_) internal::hamt_release(root_); }

    context& operator=(context o) noexcept {
        std::swap(root_, o.root_);
        return *this;
    }

    uintptr_t root() const { return root_; }

    // Snapshot the current imp's context.
    static context current() {
        context c;
        c.root_ = detail::current_imp()->dyn_ctx_;
        if (c.root_) internal::hamt_retain(c.root_);
        return c;
    }
};

// --- context_scope ---
// RAII: saves current HAMT root, restores it on destruction.
// Use for foreign-context installation. For scoped variable bindings,
// prefer csp::local.
class context_scope {
    uintptr_t saved_;
public:
    // Save current context and install a foreign one.
    explicit context_scope(const context& ctx) : saved_(detail::current_imp()->dyn_ctx_) {
        if (saved_) internal::hamt_retain(saved_);
        auto old = detail::current_imp()->dyn_ctx_;
        detail::current_imp()->dyn_ctx_ = ctx.root();
        if (ctx.root()) internal::hamt_retain(ctx.root());
        if (old) internal::hamt_release(old);
    }

    ~context_scope() {
        auto current = detail::current_imp()->dyn_ctx_;
        detail::current_imp()->dyn_ctx_ = saved_;
        if (current) internal::hamt_release(current);
    }

    context_scope(const context_scope&) = delete;
    context_scope& operator=(const context_scope&) = delete;
};

// Forward declarations for friend access.
template <typename T> class dynamic;
class local;

// --- dynamic_binding ---
// Deferred variable binding. Returned by dynamic<T>::operator=.
// Only constructible by dynamic<T>, only consumable by local.
// Asserts in destructor if not consumed (catches bare `var = val;`).
// [[nodiscard]] on operator= provides a compile-time warning regardless.
class dynamic_binding {
    friend class local;
    template <typename> friend class dynamic;

    uint64_t key_id_;
    std::any value_;
    bool consumed_ = false;

    dynamic_binding(uint64_t k, std::any v) : key_id_(k), value_(std::move(v)) {}
    dynamic_binding(dynamic_binding&& o) noexcept
        : key_id_(o.key_id_), value_(std::move(o.value_)) { o.consumed_ = true; }

public:
    dynamic_binding(const dynamic_binding&) = delete;
    dynamic_binding& operator=(const dynamic_binding&) = delete;
    dynamic_binding& operator=(dynamic_binding&&) = delete;

    ~dynamic_binding() { assert(consumed_ && "dynamic_binding destroyed without csp::local"); }
};

// --- local ---
// RAII scope for dynamic variable bindings. Saves the current HAMT root,
// applies bindings, restores on destruction.
//
//   csp::local l{depth = 42};
//   csp::local l{depth = 42, name = "x"};
//
class local {
    uintptr_t saved_;

    void apply(dynamic_binding&& b) {
        b.consumed_ = true;
        auto old = detail::current_imp()->dyn_ctx_;
        detail::current_imp()->dyn_ctx_ = internal::hamt_assoc(
            old, b.key_id_, std::move(b.value_));
        if (old) internal::hamt_release(old);
    }

public:
    template <typename... Bs>
        requires (sizeof...(Bs) >= 1 &&
                  (std::is_same_v<std::decay_t<Bs>, dynamic_binding> && ...))
    local(Bs&&... bindings) : saved_(detail::current_imp()->dyn_ctx_) {
        if (saved_) internal::hamt_retain(saved_);
        (apply(std::forward<Bs>(bindings)), ...);
    }

    ~local() {
        auto current = detail::current_imp()->dyn_ctx_;
        detail::current_imp()->dyn_ctx_ = saved_;
        if (current) internal::hamt_release(current);
    }

    local(const local&) = delete;
    local& operator=(const local&) = delete;
};

// --- dynamic<T> ---
// Typed dynamic-scoped variable. Non-copyable (unique key per instance).
// *var reads, var = val returns a dynamic_binding for use with local.
template <typename T>
class dynamic {
    context_key key_;
    std::optional<T> default_;

public:
    dynamic() {
        if constexpr (std::is_default_constructible_v<T>)
            default_.emplace();
    }
    explicit dynamic(T def) : default_(std::move(def)) {}

    dynamic(const dynamic&) = delete;
    dynamic& operator=(const dynamic&) = delete;

    // Read by value (safe, no dangling).
    T operator*() const {
        if (auto* a = internal::hamt_get(detail::current_imp()->dyn_ctx_, key_.id()))
            return *std::any_cast<T>(a);
        assert(default_.has_value());
        return *default_;
    }

    // Read by pointer into the HAMT node. Valid as long as the
    // csp::local binding is in scope.  Allows mutation and avoids
    // copying for non-trivial types.
    T* operator->() const {
        if (auto* a = internal::hamt_get(detail::current_imp()->dyn_ctx_, key_.id()))
            return const_cast<T*>(std::any_cast<T>(a));
        assert(default_.has_value());
        return const_cast<T*>(&*default_);
    }

    // Bind: returns a deferred binding for use with csp::local.
    [[nodiscard]] dynamic_binding operator=(T val) {
        return {key_.id(), std::any(std::move(val))};
    }
};

// --- imp_local<T> ---
// Imp-local variable. Like thread_local but per-imp.
// NOT inherited by child imps. Direct read/write (no local needed).
template <typename T>
class imp_local {
    context_key key_;
    std::optional<T> default_;

public:
    imp_local() {
        if constexpr (std::is_default_constructible_v<T>)
            default_.emplace();
    }
    explicit imp_local(T def) : default_(std::move(def)) {}

    imp_local(const imp_local&) = delete;
    imp_local& operator=(const imp_local&) = delete;

    // Read: map lookup + any_cast. Returns by value.
    T operator*() const {
        auto* m = detail::current_imp()->local_ctx_;
        if (m) {
            auto it = m->find(key_.id());
            if (it != m->end())
                return *std::any_cast<T>(&it->second);
        }
        assert(default_.has_value());
        return *default_;
    }

    // Write: direct mutation of per-imp map.
    imp_local& operator=(T val) {
        auto*& m = detail::current_imp()->local_ctx_;
        if (!m) m = new std::unordered_map<uint64_t, std::any>();
        (*m)[key_.id()] = std::any(std::move(val));
        return *this;
    }
};

} // namespace csp
