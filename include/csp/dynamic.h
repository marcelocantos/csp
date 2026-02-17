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

    // Snapshot the current microthread's context.
    static context current() {
        context c;
        c.root_ = detail::g_self->dyn_ctx_;
        if (c.root_) internal::hamt_retain(c.root_);
        return c;
    }
};

// --- context_scope ---
// RAII: saves current HAMT root, restores it on destruction.
class context_scope {
    uintptr_t saved_;
public:
    // Save current context (for local modifications within scope).
    context_scope() : saved_(detail::g_self->dyn_ctx_) {
        if (saved_) internal::hamt_retain(saved_);
    }

    // Save current context and install a foreign one.
    explicit context_scope(const context& ctx) : saved_(detail::g_self->dyn_ctx_) {
        if (saved_) internal::hamt_retain(saved_);
        auto old = detail::g_self->dyn_ctx_;
        detail::g_self->dyn_ctx_ = ctx.root();
        if (ctx.root()) internal::hamt_retain(ctx.root());
        if (old) internal::hamt_release(old);
    }

    ~context_scope() {
        auto current = detail::g_self->dyn_ctx_;
        detail::g_self->dyn_ctx_ = saved_;
        if (current) internal::hamt_release(current);
    }

    context_scope(const context_scope&) = delete;
    context_scope& operator=(const context_scope&) = delete;
};

// --- dynamic<T> ---
// Typed dynamic-scoped variable. Non-copyable (unique key per instance).
// *var reads, var = val writes.
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

    // Read: HAMT lookup + any_cast. Returns by value (safe, no dangling).
    T operator*() const {
        if (auto* a = internal::hamt_get(detail::g_self->dyn_ctx_, key_.id()))
            return *std::any_cast<T>(a);
        assert(default_.has_value());
        return *default_;
    }

    // Write: path-copy HAMT with new value.
    dynamic& operator=(T val) {
        auto old = detail::g_self->dyn_ctx_;
        detail::g_self->dyn_ctx_ = internal::hamt_assoc(
            old, key_.id(), std::any(std::move(val)));
        if (old) internal::hamt_release(old);
        return *this;
    }
};

} // namespace csp
