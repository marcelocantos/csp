#pragma once

// Per-worker Note: a lightweight binary semaphore that allows exactly-one-wake
// semantics. Replaces notify_all on a shared condvar (thundering herd).
//
// State machine (atomic int32_t):
//   NOTE_AWAKE   (1) — worker is running; sleep will block
//   NOTE_SLEEPING (0) — worker is blocked in sleep()
//   NOTE_FLAGGED  (2) — wakeup posted while worker was awake;
//                        next sleep() call skips the block
//
// API:
//   sleep()         — park the calling thread until wake() is called.
//                      If the note is FLAGGED, consume the flag and return
//                      immediately without blocking.
//   sleep_for(dur)  — like sleep() but with a timeout. Returns true if
//                      woken by wake(), false if timeout expired.
//   wake()          — wake the thread sleeping on this note, or set the
//                      FLAGGED state if the thread is not sleeping yet.
//   is_sleeping()   — true if this note is in SLEEPING state.
//   reset()         — restore to AWAKE; call only from the owning thread
//                      before it starts its next sleep cycle.
//
// Blocking primitive (🎯T34 O3): a single-word futex where the platform
// has one — __ulock_wait/__ulock_wake on macOS, futex(2) on Linux — so a
// wake is one syscall with no mutex or condvar involved. Windows and
// other platforms use the mutex+condvar fallback (WaitOnAddress would
// add a synchronization.lib link requirement for dist users). The
// AWAKE/SLEEPING/FLAGGED state machine is identical in both modes and
// is modeled in formal/PerWorkerWake.tla; the futex kernel-side value
// check (block only if *addr == expected) closes the CAS-then-sleep
// race the same way the condvar predicate re-check does.

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__APPLE__)
#define CSP_NOTE_FUTEX 1
extern "C" {
    // Private-but-stable Darwin ulock API (used by libc++, libdispatch).
    int __ulock_wait(uint32_t operation, void *addr, uint64_t value,
                     uint32_t timeout_us);
    int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
}
#elif defined(__linux__)
#define CSP_NOTE_FUTEX 1
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#else
#define CSP_NOTE_FUTEX 0
#include <condition_variable>
#include <mutex>
#endif

namespace csp::detail {

#if CSP_NOTE_FUTEX

namespace note_futex {

#if defined(__APPLE__)
    inline constexpr uint32_t kUlockCompareAndWait = 1;      // UL_COMPARE_AND_WAIT
    inline constexpr uint32_t kUlockNoErrno = 0x01000000;    // ULF_NO_ERRNO

    // Block while *addr == expected. timeout_us == 0 means forever.
    inline void wait(std::atomic<int32_t>* addr, int32_t expected,
                     uint32_t timeout_us) {
        __ulock_wait(kUlockCompareAndWait | kUlockNoErrno,
                     addr, uint64_t(uint32_t(expected)), timeout_us);
    }

    inline void wake(std::atomic<int32_t>* addr) {
        __ulock_wake(kUlockCompareAndWait | kUlockNoErrno, addr, 0);
    }
#else  // __linux__
    inline void wait(std::atomic<int32_t>* addr, int32_t expected,
                     uint32_t timeout_us) {
        struct timespec ts;
        struct timespec* tsp = nullptr;
        if (timeout_us != 0) {
            ts.tv_sec = timeout_us / 1'000'000u;
            ts.tv_nsec = long(timeout_us % 1'000'000u) * 1000;
            tsp = &ts;
        }
        syscall(SYS_futex, reinterpret_cast<int32_t*>(addr),
                FUTEX_WAIT_PRIVATE, expected, tsp, nullptr, 0);
    }

    inline void wake(std::atomic<int32_t>* addr) {
        syscall(SYS_futex, reinterpret_cast<int32_t*>(addr),
                FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    }
#endif

}  // namespace note_futex

#endif  // CSP_NOTE_FUTEX

class Note {
public:
    static constexpr int32_t AWAKE    = 1;
    static constexpr int32_t SLEEPING = 0;
    static constexpr int32_t FLAGGED  = 2;

    Note() : val_(AWAKE) {}

    // Called by the owning worker thread to park itself.
    // Returns immediately if the note was FLAGGED (consumes the flag).
    // Otherwise: transitions AWAKE->SLEEPING and blocks until wake().
    void sleep() noexcept {
        if (!begin_sleep()) {
            return;
        }

#if CSP_NOTE_FUTEX
        // Block until woken. The kernel re-checks val_ == SLEEPING under
        // its own lock, so a wake() that lands between our CAS and the
        // wait syscall is never lost. Loop on spurious returns.
        while (val_.load(std::memory_order_acquire) == SLEEPING) {
            note_futex::wait(&val_, SLEEPING, 0);
        }
#else
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] {
                return val_.load(std::memory_order_acquire) != SLEEPING;
            });
        }
#endif

        val_.store(AWAKE, std::memory_order_release);
    }

    // Like sleep(), but with a timeout. Returns true if woken by wake(),
    // false if the timeout expired.
    template <class Rep, class Period>
    bool sleep_for(std::chrono::duration<Rep, Period> dur) noexcept {
        if (!begin_sleep()) {
            return true;
        }

        bool woken;
#if CSP_NOTE_FUTEX
        auto deadline = std::chrono::steady_clock::now() + dur;
        // Loop with remaining-time recomputation so a spurious futex
        // return doesn't masquerade as a timeout.
        for (;;) {
            if (val_.load(std::memory_order_acquire) != SLEEPING) {
                woken = true;
                break;
            }
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                woken = false;
                break;
            }
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - now).count();
            if (us < 1) us = 1;
            if (us > 0xffffffffll) us = 0xffffffffll;
            note_futex::wait(&val_, SLEEPING, uint32_t(us));
        }
#else
        {
            std::unique_lock<std::mutex> lk(mu_);
            woken = cv_.wait_for(lk, dur, [this] {
                return val_.load(std::memory_order_acquire) != SLEEPING;
            });
        }
#endif

        val_.store(AWAKE, std::memory_order_release);
        return woken;
    }

    // Called by any thread to wake the note's owner.
    // If the owner is SLEEPING: transition SLEEPING->AWAKE and wake it.
    // If the owner is AWAKE: set FLAGGED so next sleep() returns immediately.
    // If already FLAGGED: no-op (flag already pending).
    void wake() noexcept {
        int32_t expected = SLEEPING;
        if (val_.compare_exchange_strong(expected, AWAKE,
                std::memory_order_release, std::memory_order_relaxed)) {
#if CSP_NOTE_FUTEX
            // The sleeper either hasn't entered the wait syscall yet (the
            // kernel's value check sees AWAKE and returns immediately) or
            // is blocked and this wake releases it.
            note_futex::wake(&val_);
#else
            // Notify the condvar. The sleeping thread will see val_!=SLEEPING
            // and return from cv_.wait(). Acquiring the lock first closes the
            // gap where the sleeper has evaluated the predicate as false but
            // has not yet blocked.
            std::lock_guard<std::mutex> lk(mu_);
            cv_.notify_one();
#endif
            return;
        }
        // Not SLEEPING. Try to set FLAGGED.
        expected = AWAKE;
        val_.compare_exchange_strong(expected, FLAGGED,
            std::memory_order_release, std::memory_order_relaxed);
        // If already FLAGGED, the CAS fails benignly.
    }

    // Returns true if this note is in SLEEPING state (worker is parked on it).
    bool is_sleeping() const noexcept {
        return val_.load(std::memory_order_acquire) == SLEEPING;
    }

    // Reset to AWAKE. Only the owning thread may call this before reuse.
    void reset() noexcept {
        val_.store(AWAKE, std::memory_order_relaxed);
    }

private:
    // Common entry for sleep/sleep_for: consume a pending flag (return
    // false = don't block), else transition AWAKE -> SLEEPING (return
    // true = caller blocks until val_ != SLEEPING).
    bool begin_sleep() noexcept {
        int32_t expected = FLAGGED;
        if (val_.compare_exchange_strong(expected, AWAKE,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return false;
        }

        expected = AWAKE;
        if (!val_.compare_exchange_strong(expected, SLEEPING,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            if (expected == FLAGGED) {
                val_.store(AWAKE, std::memory_order_release);
            }
            return false;
        }
        return true;
    }

    std::atomic<int32_t> val_;
#if !CSP_NOTE_FUTEX
    std::mutex mu_;
    std::condition_variable cv_;
#endif
};

}  // namespace csp::detail
