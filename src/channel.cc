#include <csp/internal/csp_internal.h>
#include <csp/internal/runtime.h>
#include <csp/ringbuffer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <random>
#include <string>


using namespace csp;
using namespace csp::detail;
using namespace csp::internal;


struct Counters {
    std::atomic<int> refs{0};
    std::atomic<int> derefs{0};
    std::atomic<int> active{0};
};

auto & counterses() {
    static Counters counterses[2] = {};
    return counterses;
}

namespace {

    class Channel;

    struct ChanopWaiter {
        ChanOp const * chanop;
        Imp * thread;

        bool operator==(ChanopWaiter const & cw) const { return chanop == cw.chanop && thread == cw.thread; }
        bool operator!=(ChanopWaiter const & cw) const { return !(*this == cw); }

        ChanopWaiter(ChanOp const * chanop, Imp * thread) : chanop{chanop}, thread{thread} { }
    };

}


namespace {

    enum {
        wr = 0,
        rd = 1
    };
    enum {
        alive_flag = 8,
        endpt_flag = 1,
        ready_flag = (ready & ~dead) << 1,
        dead_flag = dead << 1,
        ready_or_dead = ready_flag | dead_flag,
    };

    char const * descr_flags(uintptr_t waiter) {
        static char const * descrs[] = {
            "‹⋅⋅W›", "‹⋅⋅R›",
            "‹⋅⋅W›", "‹⋅⋅R›",
            "‹⋅*W›", "‹⋅*R›",
            "‹⋅*W›", "‹⋅*R›",
            "‹+⋅W›", "‹+⋅R›",
            "‹+⋅W›", "‹+⋅R›",
            "‹+*W›", "‹+*R›",
            "‹+*W›", "‹+*R›",
        };
        // Only describe non-null waiters.
        return waiter & ~uintptr_t(15) ? descrs[waiter & 15] : "";
    }

    // Extract Channel* from a Waiter/ChanOp pointer (Channel* with flags in low bits).
    Channel * get_chan(void * ptr) {
        auto p = (uintptr_t)ptr & ~15UL;
        return p ? reinterpret_cast<Channel *>(p) : nullptr;
    }

    Channel * get_chan(Waiter w) { return get_chan(w.ptr); }
    Channel * get_chan(ChanOp const & c) { return get_chan(c.waiter); }

    // Resolve a WriterRef/ReaderRef (Slot* with flags) to its current Channel*.
    Channel * get_chan_from_ref(void * ptr) {
        auto * slot = get_slot(ptr);
        return slot ? static_cast<Channel *>(slot->channel.load(std::memory_order_acquire)) : nullptr;
    }

    char const * describe(void * ch);

    class Channel {
    public:
        Channel() {
            // Must be 16-byte aligned.
            assert(((uintptr_t)this % 16) == 0);

            ++counterses()[wr].refs;
            ++counterses()[rd].refs;
            ++counterses()[wr].active;
            ++counterses()[rd].active;
        }
        ~Channel() {
        }

        void set_descr(char const * d) { descr_ = d; }

        // Check if channel is alive (both endpoint sides have live handles).
        explicit operator bool() {
            return write_slot_->refcount.load(std::memory_order_acquire) > 0
                && read_slot_->refcount.load(std::memory_order_acquire) > 0;
        }

        // Wake all waiters on a given endpoint side with a swap signal (INT_MIN).
        void wake_all_for_swap(int endpt) {
            auto & ep = endpts_[endpt];
            for (auto const & cw : ep.waiters) {
                uint32_t expected = Imp::ALT_WAITING;
                if (cw.thread->alt_state.compare_exchange_strong(expected, Imp::ALT_CLAIMED)) {
                    cw.thread->signal_ = INT_MIN;
                    cw.thread->schedule();
                }
            }
            for (auto const & cv : ep.vultures) {
                uint32_t expected = Imp::ALT_WAITING;
                if (cv.thread->alt_state.compare_exchange_strong(expected, Imp::ALT_CLAIMED)) {
                    cv.thread->signal_ = INT_MIN;
                    cv.thread->schedule();
                }
            }
        }

        // Called when a slot's endpoint refcount drops to 0.
        // endpt: wr or rd, indicating which side died.
        // The caller has already locked mu_ and verified that
        // slot->channel still points to this channel (see
        // resolve_endpoint_death).
        void on_endpoint_death_locked(int endpt) {
            ++counterses()[endpt].derefs;
            --counterses()[endpt].active;
            // Check if the opposite side has live endpoints.
            Slot * other_slot = (endpt == wr) ? read_slot_ : write_slot_;
            if (other_slot->refcount.load(std::memory_order_acquire) > 0) {
                // Wake waiters on the opposite side (death signal).
                auto & ep = endpts_[1 - endpt];
                for (auto const & cw : ep.waiters) {
                    uint32_t expected = Imp::ALT_WAITING;
                    if (cw.thread->alt_state.compare_exchange_strong(expected, Imp::ALT_CLAIMED)) {
                        int idx = int(cw.chanop - cw.thread->chanops_);
                        cw.thread->signal_ = ~idx;
                        cw.thread->schedule();
                    }
                }
                for (auto const & cv : ep.vultures) {
                    uint32_t expected = Imp::ALT_WAITING;
                    if (cv.thread->alt_state.compare_exchange_strong(expected, Imp::ALT_CLAIMED)) {
                        int idx = int(cv.chanop - cv.thread->chanops_);
                        cv.thread->signal_ = ~idx;
                        cv.thread->schedule();
                    }
                }
            }
            mu_.unlock();
            // Both endpoint sides decrement alive_. The last one
            // (fetch_sub returns 1) deletes. This avoids a race
            // when both endpoints reach refcount 0 concurrently
            // on different OS threads.
            if (alive_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // Both endpoint slots are dead. Release channel's memory
                // refs on slots (weak refs may keep slots alive longer).
                auto* ws = write_slot_;
                auto* rs = read_slot_;
                delete this;
                ws->mem_release();
                rs->mem_release();
            }
        }

        // Resolve the correct channel for a dying slot and call
        // on_endpoint_death_locked.  A concurrent swap_slots may change
        // slot->channel between the caller's fetch_sub (which observed
        // the last refcount) and our channel.load.  We lock the
        // candidate channel's mutex and re-verify; if the slot was
        // swapped away we retry with the updated channel.  The loop
        // terminates because once refcount is 0 no new swap can target
        // this slot (swap requires a live handle).
        // TLA:SwapRace.ResolveAndNotify
        static void resolve_endpoint_death(Slot * slot, int endpt) {
            for (;;) {
                auto * ch = static_cast<Channel *>(
                    slot->channel.load(std::memory_order_acquire));
                ch->mu_.lock();
                // Re-verify: swap_slots holds ch->mu_ while writing
                // slot->channel, so after acquiring mu_ the pointer
                // is stable.
                if (static_cast<Channel *>(
                        slot->channel.load(std::memory_order_acquire)) == ch) {
                    ch->on_endpoint_death_locked(endpt);  // unlocks mu_
                    return;
                }
                ch->mu_.unlock();
            }
        }

        // Internal state stored in AltMatch::opaque_.
        struct match_internal {
            Channel* fixed_sorted[8];
            Channel** sorted;       // points to fixed_sorted or heap_alloc
            Channel** heap_alloc;   // non-null if heap-allocated
            int n_sorted;
            Imp* peer;
            bool needs_unlock;
            bool use_run;           // single-P writer: unlock then run
        };
        static_assert(sizeof(match_internal) <= 128, "match_internal too large for opaque_");

        static void prialt_begin_impl(AltMatch * out, ChanOp const * chanops, int count, bool nowait, int offset = 0) {
            // Reclaim unused stack pages at this API boundary.
            if (g_imp->stk_) {
                StackPool::instance().maybe_shrink(
                    g_imp->stk_, CSP_FRAME_ADDRESS());
            }

        retry:
            // Re-resolve each chanop's Channel* through its Slot to ensure
            // the pointer is fresh.  A swap or endpoint death on another
            // thread may have changed (or deleted) the channel since the
            // Waiter was originally constructed.
            for (int i = 0; i < count; ++i) {
                if (chanops[i].slot) {
                    auto * slot = static_cast<Slot *>(chanops[i].slot);
                    auto * new_ch = slot->channel.load(std::memory_order_acquire);
                    auto flags = (uintptr_t)chanops[i].waiter.ptr & 15UL;
                    const_cast<ChanOp &>(chanops[i]).waiter.ptr =
                        (void *)((uintptr_t)new_ch | flags);
                }
            }

            auto * mi = reinterpret_cast<match_internal *>(out->opaque_);
            mi->heap_alloc = nullptr;
            mi->peer = nullptr;
            mi->needs_unlock = false;
            mi->use_run = false;
            out->src = nullptr;
            out->dst = nullptr;

            out->result = 0;

            // Collect unique channels, sorted by id for lock ordering.
            Channel* fixed_chans[8];
            std::vector<Channel*> variable_chans;
            Channel** sorted = fixed_chans;
            int n_sorted = 0;
            for (int i = 0; i < count; ++i) {
                if (Channel * ch = get_chan(chanops[i])) {
                    if (n_sorted < 8) {
                        fixed_chans[n_sorted++] = ch;
                    } else {
                        if (n_sorted == 8) {
                            variable_chans.assign(fixed_chans, fixed_chans + 8);
                        }
                        variable_chans.push_back(ch);
                        sorted = variable_chans.data();
                        n_sorted++;
                    }
                }
            }
            std::sort(sorted, sorted + n_sorted,
                      [](Channel * a, Channel * b) { return a->id_ < b->id_; });
            n_sorted = int(std::unique(sorted, sorted + n_sorted) - sorted);

            // Copy sorted channels into persistent storage.
            if (n_sorted <= 8) {
                memcpy(mi->fixed_sorted, sorted, n_sorted * sizeof(Channel*));
                mi->sorted = mi->fixed_sorted;
            } else {
                mi->heap_alloc = new Channel*[n_sorted];
                memcpy(mi->heap_alloc, sorted, n_sorted * sizeof(Channel*));
                mi->sorted = mi->heap_alloc;
            }
            mi->n_sorted = n_sorted;

            auto lock_all = [&]{ for (int i = 0; i < mi->n_sorted; ++i) mi->sorted[i]->mu_.lock(); };
            auto unlock_all = [&]{ for (int i = 0; i < mi->n_sorted; ++i) mi->sorted[i]->mu_.unlock(); };

            lock_all(); // TLA:ChannelLifecycle.WaiterAcquire

            // Verify channels haven't changed (concurrent swap_slots).
            {
                bool stale = false;
                for (int i = 0; i < count && !stale; ++i) {
                    if (chanops[i].slot) {
                        auto * slot = static_cast<Slot *>(chanops[i].slot);
                        auto * now = slot->channel.load(std::memory_order_acquire);
                        if (now != get_chan(chanops[i])) {
                            stale = true;
                        }
                    }
                }
                if (stale) {
                    unlock_all();
                    delete[] mi->heap_alloc;
                    goto retry;
                }
            }

            // TLA:ChannelLifecycle.WaiterPhase1
            // Phase 1: Scan for ready peer (priority order, rotated by offset).
            // For data chanops (ready_flag), a ready peer takes priority
            // over dead-channel detection on other channels.  For vultures
            // (~ch, dead_flag only), dead is the expected signal — fire
            // immediately.
            bool all_null = true;
            int dead_data_result = 0;  // first dead data-chanop, 0 = none yet
            for (int k = 0 ; k < count ; ++k) {
                int i = (offset + k) % count;
                auto const & chop = chanops[i];
                if (Channel * ch = get_chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter.ptr;
                    int endpt = flags & endpt_flag;

                    if (!*ch) {
                        if (flags & ready_flag) {
                            // Data chanop on dead channel: defer until
                            // after scanning for ready peers elsewhere.
                            if (!dead_data_result) dead_data_result = ~i;
                        } else {
                            // Vulture: dead is the expected signal.
                            unlock_all();
                            out->result = ~i;
                            return;
                        }
                        continue;
                    }

                    auto & them = ch->endpts_[1 - endpt].waiters;
                    if ((flags & ready_flag)) {
                        // TLA:AltStateCAS.WakerStart TLA:AltStateCAS.WakerCAS
                        for (auto & cw : them) {
                            uint32_t expected = Imp::ALT_WAITING;
                            if (cw.thread->alt_state.compare_exchange_strong(expected, Imp::ALT_CLAIMED)) {
                                int idx = int(cw.chanop - cw.thread->chanops_);
                                cw.thread->signal_ = idx;

                                // Set up match: src is always writer's
                                // buffer, dst is always reader's buffer.
                                if (endpt == wr) {
                                    out->src = chop.message;
                                    out->dst = const_cast<void *>(cw.chanop->message);
                                    mi->use_run = !Runtime::instance().mn_mode_;
                                } else {
                                    out->src = cw.chanop->message;
                                    out->dst = const_cast<void *>(chop.message);
                                }


                                out->result = i;
                                mi->peer = cw.thread;
                                mi->needs_unlock = true;
                                return;  // locks held
                            }
                        }
                    }
                    all_null = false;
                }
            }

            if (dead_data_result) {
                unlock_all();
                out->result = dead_data_result;
                return;
            }

            if (all_null || nowait) {
                unlock_all();
                if (nowait) out->result = INT_MIN;
                return;
            }

            // Phase 2: Register on all channels and sleep.
            // TLA:ChannelLifecycle.RegisterWaiter TLA:AltStateCAS.WaiterRegister
            g_imp->alt_state.store(Imp::ALT_WAITING, std::memory_order_release);
            for (int i = 0; i < count; ++i) {
                auto const & chop = chanops[i];
                if (Channel * ch = get_chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter.ptr;
                    ch->endpts_[flags & endpt_flag].wait(&chop);
                }
            }

            // Pin all unique channels so they cannot be deleted while
            // we sleep.  After wakeup, Phase 3 can safely lock and
            // deregister; the unpin after Phase 3 may trigger deferred
            // deletion if the channel's endpoints died in the interim.
            for (int i = 0; i < mi->n_sorted; ++i) {
                mi->sorted[i]->alive_.fetch_add(1, std::memory_order_relaxed);
            }

            g_imp->chanops_ = chanops;
            g_imp->n_chanops_ = count;
            // Mark suspending_ before unlock_all so that schedule()
            // (called by a waker on another thread) will set
            // wake_pending_ instead of pushing to the global queue.
            // Without this, there is a race: after unlock_all but
            // before do_switch completes, a waker could push us to
            // the global queue and a worker could run us while we
            // haven't finished suspending — double execution.
            // TLA:ChannelLifecycle.WaiterSleep TLA:DrainSuspended.BeginSuspend
            g_imp->suspending_.store(true, std::memory_order_release);
            unlock_all();
            do_switch(Status::detach);
            g_imp->suspending_.store(false, std::memory_order_release); // TLA:DrainSuspended.ClearSusp

            // Phase 3: Woken up — clean up registrations under sorted locks.
            lock_all(); // TLA:ChannelLifecycle.WaiterWakeAcquire
            for (int i = 0; i < g_imp->n_chanops_; ++i) {
                auto const & chop = g_imp->chanops_[i];
                if (Channel * ch = get_chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter.ptr;
                    ch->endpts_[flags & endpt_flag].remove(&chop, g_imp);
                }
            }
            unlock_all(); // TLA:ChannelLifecycle.WaiterCleanup

            // Unpin channels.  If an endpoint died while we slept,
            // alive_ may reach 0 here, triggering deferred deletion.
            for (int i = 0; i < mi->n_sorted; ++i) {
                Channel * ch = mi->sorted[i];
                if (ch->alive_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    auto* ws = ch->write_slot_;
                    auto* rs = ch->read_slot_;
                    delete ch;
                    ws->mem_release();
                    rs->mem_release();
                }
            }

            g_imp->alt_state.store(Imp::ALT_IDLE, std::memory_order_release); // TLA:AltStateCAS.WaiterDone

            // Check for swap wake-up: if signal_ is INT_MIN, a channel swap
            // occurred. Re-resolve happens at retry: label.
            if (g_imp->signal_ == INT_MIN) {
                g_imp->chanops_ = nullptr;
                g_imp->n_chanops_ = 0;
                delete[] mi->heap_alloc;
                mi->heap_alloc = nullptr;
                goto retry;
            }

            out->result = g_imp->signal_;
            g_imp->chanops_ = nullptr;
            g_imp->n_chanops_ = 0;
            // src/dst/peer remain null — transfer was done by the waker.
        }

        static void alt_end_impl(AltMatch * m) {
            auto * mi = reinterpret_cast<match_internal *>(m->opaque_);
            if (mi->needs_unlock) {
                if (mi->use_run) {
                    // Single-P writer path: unlock first, then context-switch
                    // to peer (runs until it yields, then returns here).
                    for (int i = 0; i < mi->n_sorted; ++i) mi->sorted[i]->mu_.unlock();
                    mi->peer->run(Status::run);
                } else {
                    if (mi->peer) mi->peer->schedule();
                    for (int i = 0; i < mi->n_sorted; ++i) mi->sorted[i]->mu_.unlock();
                }
            }
            delete[] mi->heap_alloc;
        }


    private:
        using Waiters = detail::RingBuffer<ChanopWaiter>;
        using Vultures = detail::RingBuffer<ChanopWaiter>;

        size_t id_ = []{ static std::atomic<size_t> last{0}; return ++last; }();
        std::string descr_ = [this]{ char b[25]; snprintf(b, sizeof(b), "▸%lu", id_); return std::string(b); }();
        std::atomic<int> alive_{2};  // endpoints (2) + sleeping waiters; last to 0 deletes
        std::mutex mu_;
        Slot * write_slot_ = nullptr;   // back-pointer to write endpoint slot
        Slot * read_slot_ = nullptr;    // back-pointer to read endpoint slot
        struct EndPoint {
            Waiters waiters;
            Vultures vultures;

            void wait(ChanOp const * chop) {
                auto flags = (uintptr_t)chop->waiter.ptr;
                if (flags & ready_flag) {
                    waiters.emplace(chop, g_imp);
                } else {
                    vultures.emplace(chop, g_imp);
                }
            }

            void remove(ChanOp const * chop, Imp * t) {
                auto flags = (uintptr_t)chop->waiter.ptr;
                if (flags & ready_flag) {
                    waiters.remove({chop, t});
                } else {
                    vultures.remove({chop, t});
                }
            }
        } endpts_[2];

        friend char const * describe(void *);
        friend bool csp::internal::make_chan(WriterRef*, ReaderRef*);
        friend void csp::internal::swap_slots(void*, void*);
    };

    char const * describe(void * ptr) {
        auto p = (uintptr_t)ptr & ~15UL;
        if (p) {
            auto * ch = reinterpret_cast<Channel *>(p);
            return ch->descr_.c_str();
        }
        return "▸Ø";
    }

}


namespace csp::internal {

int channel_count(int endpt) {
    auto & c = counterses()[endpt];
    return c.active - 1; // Exclude skip and global_exception_handler from the reader count.
}

bool make_chan(WriterRef * w, ReaderRef * r) {
    try {
        auto ch = new Channel{};
        auto ws = new Slot{ch};
        auto rs = new Slot{ch};
        ch->write_slot_ = ws;
        ch->read_slot_ = rs;
        *w = {reinterpret_cast<void *>(ws)};
        *r = {reinterpret_cast<void *>((uintptr_t)rs | 1)};
        return true;
    } catch (std::exception const & e) {
    } catch (...) {
    }
    return false;
}

void set_chan_descr(void * ptr, char const * descr) {
    if (Channel * c = get_chan_from_ref(ptr)) {
        c->set_descr(descr);
    }
}

char const * get_chan_descr(void * ptr) {
    auto * ch = get_chan_from_ref(ptr);
    return ch ? describe(ch) : "▸Ø";
}

char const * get_chan_flags(void * ch) {
    return descr_flags((uintptr_t)ch);
}

WriterRef writer_addref(WriterRef w) {
    if (w) {
        ++counterses()[wr].refs;
        get_slot(w.ptr)->refcount.fetch_add(1, std::memory_order_relaxed);
    }
    return w;
}

void writer_release(WriterRef w) {
    if (w) {
        auto * slot = get_slot(w.ptr);
        ++counterses()[wr].derefs;
        if (slot->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Channel::resolve_endpoint_death(slot, wr);
        }
    }
}

ReaderRef reader_addref(ReaderRef r) {
    if (r) {
        ++counterses()[rd].refs;
        get_slot(r.ptr)->refcount.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

void reader_release(ReaderRef r) {
    if (r) {
        auto * slot = get_slot(r.ptr);
        ++counterses()[rd].derefs;
        if (slot->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Channel::resolve_endpoint_death(slot, rd);
        }
    }
}

void writer_weak_addref(WriterRef w) {
    if (w) get_slot(w.ptr)->mem_refcount.fetch_add(1, std::memory_order_relaxed);
}

void writer_weak_release(WriterRef w) {
    if (w) get_slot(w.ptr)->mem_release();
}

void reader_weak_addref(ReaderRef r) {
    if (r) get_slot(r.ptr)->mem_refcount.fetch_add(1, std::memory_order_relaxed);
}

void reader_weak_release(ReaderRef r) {
    if (r) get_slot(r.ptr)->mem_release();
}

bool try_upgrade_weak_writer(WriterRef w) {
    if (!w) return false;
    auto* slot = get_slot(w.ptr);
    size_t old = slot->refcount.load(std::memory_order_acquire);
    while (old > 0) {
        if (slot->refcount.compare_exchange_weak(old, old + 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            ++counterses()[wr].refs;
            return true;
        }
    }
    return false;
}

bool try_upgrade_weak_reader(ReaderRef r) {
    if (!r) return false;
    auto* slot = get_slot(r.ptr);
    size_t old = slot->refcount.load(std::memory_order_acquire);
    while (old > 0) {
        if (slot->refcount.compare_exchange_weak(old, old + 1,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            ++counterses()[rd].refs;
            return true;
        }
    }
    return false;
}

void swap_slots(void * slot_a_ptr, void * slot_b_ptr) {
    auto * sa = static_cast<Slot *>(slot_a_ptr);
    auto * sb = static_cast<Slot *>(slot_b_ptr);
    if (sa == sb) return;

    auto * ca = static_cast<Channel *>(sa->channel.load(std::memory_order_acquire));
    auto * cb = static_cast<Channel *>(sb->channel.load(std::memory_order_acquire));
    if (ca == cb) return;

    // Lock both channels in id order to avoid deadlock.
    if (ca->id_ > cb->id_) {
        std::swap(ca, cb);
        std::swap(sa, sb);
    }

    ca->mu_.lock();
    cb->mu_.lock();

    // Verify channels haven't changed (concurrent swap).
    auto * ca_now = static_cast<Channel *>(sa->channel.load(std::memory_order_acquire));
    auto * cb_now = static_cast<Channel *>(sb->channel.load(std::memory_order_acquire));
    if (ca_now != ca || cb_now != cb) {
        cb->mu_.unlock();
        ca->mu_.unlock();
        // Retry with current state.
        swap_slots(slot_a_ptr, slot_b_ptr);
        return;
    }

    // Determine which endpoint side (write or read) for each channel.
    bool a_is_write = (ca->write_slot_ == sa);
    bool b_is_write = (cb->write_slot_ == sb);
    assert(a_is_write == b_is_write && "cannot swap write slot with read slot");

    // Exchange channel pointers in slots.
    sa->channel.store(cb, std::memory_order_release);
    sb->channel.store(ca, std::memory_order_release);

    // Exchange back-pointers on channels.
    if (a_is_write) {
        ca->write_slot_ = sb;
        cb->write_slot_ = sa;
    } else {
        ca->read_slot_ = sb;
        cb->read_slot_ = sa;
    }

    // Wake all waiters on both channels (both sides) so they can
    // re-resolve their slots and detect any death-status changes.
    for (int side = 0; side < 2; ++side) {
        ca->wake_all_for_swap(side);
        cb->wake_all_for_swap(side);
    }

    cb->mu_.unlock();
    ca->mu_.unlock();
}

void prialt_begin(AltMatch * out, ChanOp const * chanops, int count, int nowait) {
    Channel::prialt_begin_impl(out, chanops, count, bool(nowait));
}

void alt_begin(AltMatch * out, ChanOp const * chanops, int count, int nowait) {
    int offset = 0;
    if (count > 1) {
        thread_local std::mt19937 rng{std::random_device{}()};
        offset = std::uniform_int_distribution<int>(0, count - 1)(rng);
    }
    Channel::prialt_begin_impl(out, chanops, count, bool(nowait), offset);
}

void alt_end(AltMatch * m) {
    Channel::alt_end_impl(m);
}

} // namespace csp::internal
