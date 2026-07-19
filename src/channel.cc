#include <csp/internal/csp_internal.h>
#include <csp/internal/runtime.h>
#include <csp/ringbuffer.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>


using namespace csp;
using namespace csp::detail;
using namespace csp::internal;


// Debug/introspection tallies (channel_count). Relaxed ordering: they
// are monotonic counters with no synchronizing role, and the default
// seq_cst RMWs put two dmb-fenced ops on a shared cache line into
// every endpoint create/copy/release (🎯T34 O7).
struct Counters {
    std::atomic<int> refs{0};
    std::atomic<int> derefs{0};
    std::atomic<int> active{0};

    void ref() { refs.fetch_add(1, std::memory_order_relaxed); }
    void deref() { derefs.fetch_add(1, std::memory_order_relaxed); }
    void activate() { active.fetch_add(1, std::memory_order_relaxed); }
    void deactivate() { active.fetch_sub(1, std::memory_order_relaxed); }
};

auto & counterses() {
    static Counters counterses[2] = {};
    return counterses;
}

namespace {

    // 🎯T29 hang diagnosis scaffolding: env-gated one-line traces of the
    // endpoint-death / registration / wake protocol. Namespace-scope
    // initializer (not a function-local static): access is a plain load
    // with no init-guard check at the ~8 DD sites on the prialt path.
    bool const g_debug_death = std::getenv("CSP_DEBUG_DEATH") != nullptr;
#define DD(...) do { if (g_debug_death) [[unlikely]] fprintf(stderr, __VA_ARGS__); } while (0)

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
        auto p = (uintptr_t)ptr & ~uintptr_t{15};
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

            counterses()[wr].ref();
            counterses()[rd].ref();
            counterses()[wr].activate();
            counterses()[rd].activate();
        }
        ~Channel() {
        }

        void set_descr(char const * d) { descr_ = d; }

        // Check if channel is alive (both endpoint sides have live handles).
        explicit operator bool() {
            return write_slot_->refcount.load(std::memory_order_acquire) > 0
                && read_slot_->refcount.load(std::memory_order_acquire) > 0;
        }

        // Claim + wake every WAITING entry of a ring, assigning signal_
        // via sig(idx). Caller holds mu_. Single-op sleepers (🎯T35
        // waker-side deregistration) are removed from the ring at claim
        // time — their only registration is on this channel, so the
        // woken side skips its phase-3 relock entirely.
        // TLA:ChannelLifecycle.WaiterWakeNoCleanup
        // On removal the ring backfills the hole, so the index is NOT
        // advanced; entries are copied out before removal.
        template <typename Ring, typename Sig>
        static void claim_wake_all(Ring & ring, Sig sig) {
            for (size_t i = 0; i < ring.count(); ) {
                auto const & cw = ring.begin()[static_cast<ptrdiff_t>(i)];
                Imp * t = cw.thread;
                ChanOp const * chop = cw.chanop;
                uint32_t expected = Imp::ALT_WAITING;
                if (!t->alt_state.compare_exchange_strong(expected, Imp::ALT_CLAIMED)) {
                    ++i;
                    continue;
                }
                t->signal_ = sig(int(chop - t->chanops_));
                bool const single = t->n_chanops_ == 1;
                if (single) {
                    ring.remove({chop, t});  // cw is dangling from here
                }
                t->make_runnable();
                if (!single) {
                    ++i;
                }
            }
        }

        // Wake all waiters on a given endpoint side with a swap signal (INT_MIN).
        void wake_all_for_swap(int endpt) {
            auto & ep = endpts_[endpt];
            claim_wake_all(ep.waiters, [](int) { return INT_MIN; });
            claim_wake_all(ep.vultures, [](int) { return INT_MIN; });
        }

        // Called when a slot's endpoint refcount drops to 0.
        // endpt: wr or rd, indicating which side died.
        // The caller has already locked mu_ and verified that
        // slot->channel still points to this channel (see
        // resolve_endpoint_death).
        void on_endpoint_death_locked(int endpt) {
            counterses()[endpt].deref();
            counterses()[endpt].deactivate();
            // Check if the opposite side has live endpoints.
            Slot * other_slot = (endpt == wr) ? read_slot_ : write_slot_;
            DD("DD death ch=%p endpt=%d other_refs=%zu\n", (void*)this, endpt,
               other_slot->refcount.load(std::memory_order_acquire));
            if (other_slot->refcount.load(std::memory_order_acquire) > 0) {
                // Wake waiters on the opposite side (death signal).
                auto & ep = endpts_[1 - endpt];
                claim_wake_all(ep.waiters, [this](int idx) {
                    DD("DD death-wake ch=%p idx=%d\n", (void*)this, idx);
                    return ~idx;
                });
                claim_wake_all(ep.vultures, [this](int idx) {
                    DD("DD death-wake-vulture ch=%p idx=%d\n", (void*)this, idx);
                    return ~idx;
                });
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
            // (removed: use_run field was for single-P cooperative path)
            bool has_pins;          // re-resolution alive_ pins active
        };
        static_assert(sizeof(match_internal) <= 128, "match_internal too large for opaque_");

        static void unpin_channel(Channel * ch) {
            if (ch->alive_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                auto* ws = ch->write_slot_;
                auto* rs = ch->read_slot_;
                delete ch;
                ws->mem_release();
                rs->mem_release();
            }
        }

        static void prialt_begin_impl(AltMatch * out, ChanOp const * chanops, int count, bool nowait, int offset = 0) {
            // Cache the current imp. The pointer stays valid across
            // do_switch below: it names this imp, and Imp::run restores
            // current_imp() to it before control returns here. Only the
            // TLS *slot* must not be cached across a switch (see
            // docs/tls-caching-bug.md); the Imp* value is stable.
            auto * const self = current_imp();

            // Reclaim unused stack pages at this API boundary.
            if (self->stk_) {
                auto* fp = CSP_FRAME_ADDRESS();
                check_stack_overflow(self, fp);
                StackPool::instance().maybe_shrink(self->stk_, fp);
            }

            auto * mi = reinterpret_cast<match_internal *>(out->opaque_);
            mi->heap_alloc = nullptr;
            mi->has_pins = false;

        retry:
            // Unpin channels from a previous iteration if active.
            if (mi->has_pins) {
                for (int i = 0; i < mi->n_sorted; ++i) unpin_channel(mi->sorted[i]);
                mi->has_pins = false;
            }
            delete[] mi->heap_alloc;
            mi->heap_alloc = nullptr;

            mi->peer = nullptr;
            mi->needs_unlock = false;
            out->src = nullptr;
            out->dst = nullptr;
            out->result = 0;

            // Re-resolve each chanop's Channel* through its Slot under the
            // slot spinlock, and build the unique channel set with alive_
            // pins.  Pinning under the spinlock prevents the channel from
            // being freed between the load and the pin (see
            // docs/papers/11-channel-reresolution-uaf.md).
            Channel* fixed_chans[8];
            std::vector<Channel*> variable_chans;
            Channel** chans = fixed_chans;
            int n_chans = 0;

            for (int i = 0; i < count; ++i) {
                Channel* new_ch;
                auto * slot = chanops[i].slot
                    ? static_cast<Slot *>(chanops[i].slot) : nullptr;

                if (slot) slot->lock();

                if (slot) {
                    new_ch = static_cast<Channel*>(
                        slot->channel.load(std::memory_order_acquire));
                    auto flags = (uintptr_t)chanops[i].waiter.ptr & uintptr_t{15};
                    const_cast<ChanOp &>(chanops[i]).waiter.ptr =
                        (void *)((uintptr_t)new_ch | flags);
                } else {
                    new_ch = get_chan(chanops[i]);
                }

                if (new_ch) {
                    // Dedup: check if already in unique set.
                    bool found = false;
                    for (int j = 0; j < n_chans; ++j) {
                        if (chans[j] == new_ch) { found = true; break; }
                    }
                    if (!found) {
                        // Pin to prevent deletion between slot unlock
                        // and lock_all (see paper §Fix).
                        new_ch->alive_.fetch_add(1, std::memory_order_relaxed);
                        if (n_chans < 8) {
                            fixed_chans[n_chans++] = new_ch;
                        } else {
                            if (n_chans == 8) {
                                variable_chans.assign(fixed_chans, fixed_chans + 8);
                            }
                            variable_chans.push_back(new_ch);
                            chans = variable_chans.data();
                            n_chans++;
                        }
                    }
                }

                if (slot) slot->unlock();
            }

            // Sort unique channels by id for lock ordering. count==1
            // (plain send/recv, the dominant call shape) skips the call.
            if (n_chans > 1) {
                std::sort(chans, chans + n_chans,
                          [](Channel * a, Channel * b) { return a->id_ < b->id_; });
            }

            // Copy into persistent storage in AltMatch opaque.
            if (n_chans <= 8) {
                memcpy(mi->fixed_sorted, chans, n_chans * sizeof(Channel*));
                mi->sorted = mi->fixed_sorted;
            } else {
                mi->heap_alloc = new Channel*[n_chans];
                memcpy(mi->heap_alloc, chans, n_chans * sizeof(Channel*));
                mi->sorted = mi->heap_alloc;
            }
            mi->n_sorted = n_chans;
            mi->has_pins = true;

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
                        DD("DD dead-scan imp=%p §%zu ch=%p i=%d ready=%d\n",
                           (void*)self, self->id_, (void*)ch,
                           i, int(flags & ready_flag));
                        if (flags & ready_flag) {
                            // Data chanop on dead channel: defer until
                            // after scanning for ready peers elsewhere.
                            if (!dead_data_result) dead_data_result = ~i;
                        } else {
                            // Vulture: dead is the expected signal.
                            unlock_all();
                            for (int j = 0; j < mi->n_sorted; ++j) unpin_channel(mi->sorted[j]);
                            mi->has_pins = false;
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
                                Imp * peer = cw.thread;
                                ChanOp const * peer_chop = cw.chanop;
                                int idx = int(peer_chop - peer->chanops_);
                                peer->signal_ = idx;
                                DD("DD match matcher=%p §%zu claimed=%p §%zu ch=%p idx=%d\n",
                                   (void*)self, self->id_,
                                   (void*)peer, peer->id_, (void*)ch, idx);

                                // Set up match: src is always writer's
                                // buffer, dst is always reader's buffer.
                                // eptr_dst comes from the reader side (its
                                // optional std::exception_ptr storage).
                                if (endpt == wr) {
                                    out->src = chop.message;
                                    out->dst = const_cast<void *>(peer_chop->message);
                                    out->eptr_dst = peer_chop->eptr_dst;
                                } else {
                                    out->src = peer_chop->message;
                                    out->dst = const_cast<void *>(chop.message);
                                    out->eptr_dst = chop.eptr_dst;
                                }

                                // 🎯T35 waker-side deregistration: a
                                // single-op sleeper's only registration is
                                // on this channel — remove it now (under
                                // mu_) so the woken side skips its phase-3
                                // relock. cw dangles after the remove.
                                // TLA:ChannelLifecycle.WaiterWakeNoCleanup
                                if (peer->n_chanops_ == 1) {
                                    them.remove({peer_chop, peer});
                                }

                                out->result = i;
                                mi->peer = peer;
                                mi->needs_unlock = true;
                                return;  // locks + pins held → alt_end_impl unpins
                            }
                        }
                    }
                    all_null = false;
                }
            }

            if (dead_data_result) {
                unlock_all();
                for (int i = 0; i < mi->n_sorted; ++i) unpin_channel(mi->sorted[i]);
                mi->has_pins = false;
                out->result = dead_data_result;
                return;
            }

            if (all_null || nowait) {
                unlock_all();
                for (int i = 0; i < mi->n_sorted; ++i) unpin_channel(mi->sorted[i]);
                mi->has_pins = false;
                if (nowait) out->result = INT_MIN;
                return;
            }

            // Phase 2: Register on all channels and sleep.
            // Re-resolution pins (alive_++) serve double duty: they keep
            // channels alive while we sleep.  No separate pin loop needed.
            // TLA:ChannelLifecycle.RegisterWaiter TLA:AltStateCAS.WaiterRegister
            self->alt_state.store(Imp::ALT_WAITING, std::memory_order_release);
            for (int i = 0; i < count; ++i) {
                auto const & chop = chanops[i];
                if (Channel * ch = get_chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter.ptr;
                    DD("DD reg imp=%p §%zu ch=%p endpt=%d i=%d ready=%d\n",
                       (void*)self, self->id_, (void*)ch,
                       int(flags & endpt_flag), i, int(flags & ready_flag));
                    ch->endpts_[flags & endpt_flag].wait(&chop, self);
                }
            }

            self->chanops_ = chanops;
            self->n_chanops_ = count;
            // Announce the suspend window before unlock_all so that
            // schedule() (called by a waker on another thread) defers
            // the wake (CAS to SUSP_WAKE) instead of pushing to a run
            // queue. Without this, there is a race: after unlock_all
            // but before do_switch completes, a waker could push us to
            // the global queue and a worker could run us while we
            // haven't finished suspending — double execution.
            // TLA:ChannelLifecycle.WaiterSleep TLA:DrainSuspended.BeginSuspend
            DD("DD suspend imp=%p §%zu\n",
               (void*)self, self->id_);
            self->suspend_state_.store(Imp::SUSP_PENDING, std::memory_order_release);
            unlock_all();
            do_switch(Status::detach, self);
            // suspend_state_ is SUSP_IDLE here: CheckWP (early wake) or
            // drain_suspended (context switch) reset it.
            DD("DD woke imp=%p §%zu signal=%d\n",
               (void*)self, self->id_, self->signal_);

            // Phase 3: Woken up — clean up registrations under sorted
            // locks. Single-op fast path (🎯T35): the claimer removed
            // our only registration while holding that channel's lock,
            // so there is nothing to relock or scan.
            // TLA:ChannelLifecycle.WaiterWakeNoCleanup
            if (count > 1) {
                lock_all();
                for (int i = 0; i < self->n_chanops_; ++i) {
                    auto const & chop = self->chanops_[i];
                    if (Channel * ch = get_chan(chop)) {
                        auto flags = (uintptr_t)chop.waiter.ptr;
                        ch->endpts_[flags & endpt_flag].remove(&chop, self);
                    }
                }
                unlock_all();
            }

            // Unpin channels.  If an endpoint died while we slept,
            // alive_ may reach 0 here, triggering deferred deletion.
            for (int i = 0; i < mi->n_sorted; ++i) unpin_channel(mi->sorted[i]);
            mi->has_pins = false;

            self->alt_state.store(Imp::ALT_IDLE, std::memory_order_release); // TLA:AltStateCAS.WaiterDone

            // Check for swap wake-up: if signal_ is INT_MIN, a channel swap
            // occurred. Re-resolve happens at retry: label.
            if (self->signal_ == INT_MIN) {
                self->chanops_ = nullptr;
                self->n_chanops_ = 0;
                goto retry;
            }

            out->result = self->signal_;
            self->chanops_ = nullptr;
            self->n_chanops_ = 0;
            // src/dst/peer remain null — transfer was done by the waker.
        }

        static void alt_end_impl(AltMatch * m) {
            auto * mi = reinterpret_cast<match_internal *>(m->opaque_);
            if (mi->needs_unlock) {
                for (int i = 0; i < mi->n_sorted; ++i) mi->sorted[i]->mu_.unlock();
            }
            if (mi->has_pins) {
                for (int i = 0; i < mi->n_sorted; ++i) unpin_channel(mi->sorted[i]);
            }
            auto* peer = mi->peer;
            delete[] mi->heap_alloc;
            if (peer) {
                DD("DD alt-end by=%p §%zu wakes peer=%p §%zu\n",
                   (void*)current_imp(), current_imp()->id_,
                   (void*)peer, peer->id_);
                peer->make_runnable();
            }
        }


    private:
        using Waiters = detail::RingBuffer<ChanopWaiter>;
        using Vultures = detail::RingBuffer<ChanopWaiter>;

        size_t id_ = []{ static std::atomic<size_t> last{0}; return ++last; }();
        // Empty until set_descr(): the default "▸N" form is formatted on
        // demand in describe() (debug paths only) instead of paying a
        // snprintf + string per channel create (🎯T35: ~37 of 156 ns).
        std::string descr_;
        std::atomic<int> alive_{2};  // endpoints (2) + sleeping waiters; last to 0 deletes
        FastMutex mu_;
        Slot * write_slot_ = nullptr;   // back-pointer to write endpoint slot
        Slot * read_slot_ = nullptr;    // back-pointer to read endpoint slot
        struct EndPoint {
            Waiters waiters;
            Vultures vultures;

            void wait(ChanOp const * chop, Imp * t) {
                auto flags = (uintptr_t)chop->waiter.ptr;
                if (flags & ready_flag) {
                    waiters.emplace(chop, t);
                } else {
                    vultures.emplace(chop, t);
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
        auto p = (uintptr_t)ptr & ~uintptr_t{15};
        if (p) {
            auto * ch = reinterpret_cast<Channel *>(p);
            if (!ch->descr_.empty()) {
                return ch->descr_.c_str();
            }
            // Default "▸N" form, formatted on demand (debug paths only).
            thread_local char buf[25];
            snprintf(buf, sizeof(buf), "▸%zu", ch->id_);
            return buf;
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
    } catch (std::exception const &) {
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
        counterses()[wr].ref();
        get_slot(w.ptr)->refcount.fetch_add(1, std::memory_order_relaxed);
    }
    return w;
}

void writer_release(WriterRef w) {
    if (w) {
        auto * slot = get_slot(w.ptr);
        counterses()[wr].deref();
        if (slot->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Channel::resolve_endpoint_death(slot, wr);
        }
    }
}

ReaderRef reader_addref(ReaderRef r) {
    if (r) {
        counterses()[rd].ref();
        get_slot(r.ptr)->refcount.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

void reader_release(ReaderRef r) {
    if (r) {
        auto * slot = get_slot(r.ptr);
        counterses()[rd].deref();
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
            counterses()[wr].ref();
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
            counterses()[rd].ref();
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
    [[maybe_unused]] bool a_is_write = (ca->write_slot_ == sa);
    [[maybe_unused]] bool b_is_write = (cb->write_slot_ == sb);
    assert(a_is_write == b_is_write && "cannot swap write slot with read slot");

    // Exchange channel pointers in slots.
    // Hold slot spinlock to serialize with re-resolution pinning
    // in prialt_begin_impl (see docs/papers/11-*.md).
    sa->lock();
    sa->channel.store(cb, std::memory_order_release);
    sa->unlock();

    sb->lock();
    sb->channel.store(ca, std::memory_order_release);
    sb->unlock();

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
        // xorshift64* + Lemire reduction: the offset only needs to be
        // uniform-ish for fairness, not cryptographic, and this stays
        // off the syscall/heavy-math path (mt19937 + rejection sampling
        // cost ~25 ns per alt).
        thread_local uint64_t rng_state =
            0x9e3779b97f4a7c15u ^ reinterpret_cast<uintptr_t>(&rng_state);
        uint64_t x = rng_state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        rng_state = x;
        offset = int(((x * 0x2545f4914f6cdd1du >> 32) * uint64_t(count)) >> 32);
    }
    Channel::prialt_begin_impl(out, chanops, count, bool(nowait), offset);
}

void alt_end(AltMatch * m) {
    Channel::alt_end_impl(m);
}

} // namespace csp::internal
