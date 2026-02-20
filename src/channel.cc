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

    Channel * get_chan(void * ptr) {
        if (auto cp = reinterpret_cast<Channel * *>((uintptr_t)ptr & ~15UL)) {
            return *cp;
        }
        return nullptr;
    }

    Channel * get_chan(WriterRef w) { return get_chan(w.ptr); }
    Channel * get_chan(ReaderRef r) { return get_chan(r.ptr); }
    Channel * get_chan(Waiter w) { return get_chan(w.ptr); }
    Channel * get_chan(ChanOp const & c) { return get_chan(c.waiter); }

    char const * describe(void * ch);

    class Channel {
    public:
        Channel() {
            static_assert(offsetof(Channel,delegate_) == 0, "delegate_ must be at the start for get_chan() to work");
            // Must be 16-byte aligned.
            assert(((uintptr_t)this % 16) == 0);

            ++counterses()[wr].refs;
            ++counterses()[rd].refs;
            ++counterses()[wr].active;
            ++counterses()[rd].active;
        }
        ~Channel() {
        }
        WriterRef as_writer() { return {reinterpret_cast<void*>(this)}; }
        ReaderRef as_reader() { return {reinterpret_cast<void*>((uintptr_t)this | 1)}; }
        void set_descr(char const * d) { descr_ = d; }

        void addref(int endpt) {
            ++counterses()[endpt].refs;
            endpts_[endpt].refcount.fetch_add(1, std::memory_order_relaxed);
        }
        // TLA:ChannelLifecycle.CloserWDecRef TLA:ChannelLifecycle.CloserRDecRef (shared: endpt selects W or R)
        void release(int endpt) {
            ++counterses()[endpt].derefs;
            if (endpts_[endpt].refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                {
                    // TLA:ChannelLifecycle.CloserWAcquire TLA:ChannelLifecycle.CloserRAcquire
                    std::lock_guard<std::mutex> lock(mu_);
                    --counterses()[endpt].active;
                    auto & ep = endpts_[1 - endpt];
                    if (ep.refcount.load(std::memory_order_acquire) > 0) {
                        // TLA:ChannelLifecycle.CloserWWakeWaiters TLA:ChannelLifecycle.CloserRWakeWaiters
                        // TLA:AltStateCAS.WakerCAS TLA:AltStateCAS.WakerSetSignal TLA:AltStateCAS.WakerSchedule TLA:AltStateCAS.LoserDone
                        // TLA:DrainSuspended.StartWake
                        // Wake waiters via CAS. Don't remove from queues —
                        // woken threads clean up their own registrations.
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
                        // Don't clear — woken threads clean up their own registrations.
                    }
                } // TLA:ChannelLifecycle.CloserWReleaseMu TLA:ChannelLifecycle.CloserRReleaseMu
                // TLA:ChannelLifecycle.CloserWDecAlive TLA:ChannelLifecycle.CloserRDecAlive
                // Both endpoint sides decrement alive_. The last one
                // (fetch_sub returns 1) deletes. This avoids a race
                // when both endpoints reach refcount 0 concurrently
                // on different OS threads.
                if (alive_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    delete this;
                }
            }
        }

        explicit operator bool() { return endpts_[wr].refcount.load() > 0 && endpts_[rd].refcount.load() > 0; }

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
                    g_imp->stk_, __builtin_frame_address(0));
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

            g_imp->alt_state.store(Imp::ALT_IDLE, std::memory_order_release); // TLA:AltStateCAS.WaiterDone
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

        // Anticipate channel fusing capability.
        Channel * delegate_ = this;

        size_t id_ = []{ static std::atomic<size_t> last{0}; return ++last; }();
        std::string descr_ = [this]{ char b[25]; snprintf(b, sizeof(b), "▸%lu", id_); return std::string(b); }();
        std::atomic<int> alive_{2};  // one per endpoint side; last to 0 deletes
        std::mutex mu_;
        struct EndPoint {
            std::atomic<size_t> refcount{1};
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
    };

    char const * describe(void * ch) {
        if (Channel * c = get_chan(ch)) {
            return c->descr_.c_str();
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
        *w = ch->as_writer();
        *r = ch->as_reader();
        return true;
    } catch (std::exception const & e) {
    } catch (...) {
    }
    return false;
}

void set_chan_descr(void * ch, char const * descr) {
    if (Channel * c = get_chan(ch)) {
        c->set_descr(descr);
    }
}

char const * get_chan_descr(void * ch) {
    return describe(ch);
}

char const * get_chan_flags(void * ch) {
    return descr_flags((uintptr_t)ch);
}

WriterRef writer_addref(WriterRef w) { if (w) get_chan(w)->addref(wr); return w; }
void      writer_release(WriterRef w) { if (w) get_chan(w)->release(wr); }
ReaderRef reader_addref(ReaderRef r) { if (r) get_chan(r)->addref(rd); return r; }
void      reader_release(ReaderRef r) { if (r) get_chan(r)->release(rd); }

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
