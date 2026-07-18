#pragma once

// FastMutex: low-overhead mutual exclusion for short critical sections
// (🎯T34 round 2). The scheduler's hot locks (channel mu_, per-P
// run_mu, global_mu) are held for tens of nanoseconds with no
// syscalls, no allocation on the common path, and never across a
// context switch — but they were std::mutex, which on macOS is a full
// pthread_mutex (~3× the uncontended cost of os_unfair_lock and a
// heavier contended path).
//
// macOS: os_unfair_lock — kernel-assisted (no userspace spin storm),
// priority-donating, with trylock. Constraints honored by all users:
// not recursive, unlocked by the locking thread (no lock is ever held
// across jump_fcontext, so imp migration cannot split a lock/unlock
// pair across OS threads).
//
// Linux: std::mutex is already a lightweight futex lock in glibc;
// Windows MSVC STL uses SRWLOCK. The fallback alias is fine there.
//
// NOTE: no condition_variable may wait on a FastMutex — park_mu stays
// std::mutex for exactly that reason.

#if defined(__APPLE__)

#include <os/lock.h>

namespace csp::detail {

class FastMutex {
public:
    FastMutex() = default;
    FastMutex(FastMutex const&) = delete;
    FastMutex& operator=(FastMutex const&) = delete;

    void lock() noexcept { os_unfair_lock_lock(&l_); }
    bool try_lock() noexcept { return os_unfair_lock_trylock(&l_); }
    void unlock() noexcept { os_unfair_lock_unlock(&l_); }

private:
    os_unfair_lock l_ = OS_UNFAIR_LOCK_INIT;
};

}  // namespace csp::detail

#else

#include <mutex>

namespace csp::detail {

using FastMutex = std::mutex;

}  // namespace csp::detail

#endif
