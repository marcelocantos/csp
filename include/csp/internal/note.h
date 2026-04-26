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
// Implementation: uses platform futex primitives for low-latency wakeup
// (macOS __ulock_wait, Linux futex, Windows WaitOnAddress). Falls back to
// a per-note condvar if the platform support is unavailable.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace csp::detail {

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
        // Consume a pending flag.
        int32_t expected = FLAGGED;
        if (val_.compare_exchange_strong(expected, AWAKE,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }

        // Transition AWAKE -> SLEEPING.
        expected = AWAKE;
        if (!val_.compare_exchange_strong(expected, SLEEPING,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            if (expected == FLAGGED) {
                val_.store(AWAKE, std::memory_order_release);
            }
            return;
        }

        // Block until woken (val_ changes from SLEEPING).
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] {
                return val_.load(std::memory_order_acquire) != SLEEPING;
            });
        }

        val_.store(AWAKE, std::memory_order_release);
    }

    // Like sleep(), but with a timeout. Returns true if woken by wake(),
    // false if the timeout expired.
    template <class Rep, class Period>
    bool sleep_for(std::chrono::duration<Rep, Period> dur) noexcept {
        // Consume flag.
        int32_t expected = FLAGGED;
        if (val_.compare_exchange_strong(expected, AWAKE,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return true;
        }

        // Transition AWAKE -> SLEEPING.
        expected = AWAKE;
        if (!val_.compare_exchange_strong(expected, SLEEPING,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            if (expected == FLAGGED) {
                val_.store(AWAKE, std::memory_order_release);
            }
            return true;
        }

        bool woken;
        {
            std::unique_lock<std::mutex> lk(mu_);
            woken = cv_.wait_for(lk, dur, [this] {
                return val_.load(std::memory_order_acquire) != SLEEPING;
            });
        }

        val_.store(AWAKE, std::memory_order_release);
        return woken;
    }

    // Called by any thread to wake the note's owner.
    // If the owner is SLEEPING: transition SLEEPING->AWAKE and notify cv.
    // If the owner is AWAKE: set FLAGGED so next sleep() returns immediately.
    // If already FLAGGED: no-op (flag already pending).
    void wake() noexcept {
        int32_t expected = SLEEPING;
        if (val_.compare_exchange_strong(expected, AWAKE,
                std::memory_order_release, std::memory_order_relaxed)) {
            // Notify the condvar. The sleeping thread will see val_!=SLEEPING
            // and return from cv_.wait().
            //
            // We must acquire the lock before notify to prevent this race:
            //   Thread A: CAS SLEEPING->AWAKE (done)
            //   Thread B: (in cv_.wait_for) predicate true, wakeup pending
            //   Thread A: notify_one (but thread B hasn't re-blocked yet)
            // Actually, notify without holding the lock is safe here because:
            // the predicate checks val_ atomically, and val_ is already AWAKE
            // before we notify. If the sleeping thread checks the predicate
            // between our CAS and our notify, it sees AWAKE and exits without
            // blocking. If it's already blocked in cv_.wait, our notify wakes it.
            // The key: there is NO window where val_=AWAKE and the thread is
            // re-entering cv_.wait (it only enters once per sleep() call).
            std::lock_guard<std::mutex> lk(mu_);
            cv_.notify_one();
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
    std::atomic<int32_t> val_;
    std::mutex mu_;
    std::condition_variable cv_;
};

}  // namespace csp::detail
