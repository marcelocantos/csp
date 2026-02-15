#include <csp/internal/microthread_internal.h>
#include <csp/internal/runtime.h>
#include <csp/ringbuffer.h>

#include <boost/range/iterator_range_core.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <stdexcept>
#include <sstream>
#include <string>
#include <unordered_set>

using namespace csp;
using namespace csp::detail;

static Logger g_lifespan  ("channel/lifespan");
static Logger g_chlog     ("channel/basic");
static Logger g_verboselog("channel/verbose");
static Logger g_sleeplog  ("channel/sleep");
static Logger g_msglog    ("channel/msg");
static Logger g_debug     ("channel/debug");
static Logger g_sequence  ("microthread/sequence");

static std::mutex g_chdescrs_mu;
static std::map<void *, std::string> g_chdescrs;

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
        csp_chanop const * chanop;
        Microthread * thread;

        bool operator==(ChanopWaiter const & cw) const { return chanop == cw.chanop && thread == cw.thread; }
        bool operator!=(ChanopWaiter const & cw) const { return !(*this == cw); }

        ChanopWaiter(csp_chanop const * chanop, Microthread * thread) : chanop{chanop}, thread{thread} { }
    };

}

namespace std {

    template <>
    struct hash<ChanopWaiter> {
        size_t operator()(ChanopWaiter const & cw) const {
            return size_t(9018121390601033611UL) * hash<void const *>{}(cw.chanop) + hash<void const *>{}(cw.thread);
        }
    };

}

namespace {

    enum {
        wr = 0,
        rd = 1
    };
    enum {
        csp_alive_flag = 8,
        csp_endpt_flag = 1,
        csp_ready_flag = (csp_ready & ~csp_dead) << 1,
        csp_dead_flag = csp_dead << 1,
        csp_ready_or_dead = csp_ready_flag | csp_dead_flag,
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

    template <typename T>
    Channel * chan(T t) {
        if (auto cp = reinterpret_cast<Channel * *>((uintptr_t)t & ~15UL)) {
            return *cp;
        }
        return nullptr;
    }

    Channel * chan(csp_chanop const & c) {
        return chan(c.waiter);
    }

    char const * describe(void * ch);

    class Channel {
    public:
        Channel(void (* tx)(void * src, void * dst)) : tx_(tx) {     CSP_LOG(g_verboselog, "new (%s[%zu:%zu]) Channel", describe(this), endpts_[0].refcount.load(), endpts_[1].refcount.load());
            static_assert(offsetof(Channel,delegate_) == 0, "delegate_ must be at the start for chan() to work");
            // Must be 16-byte aligned.
            assert(((uintptr_t)this % 16) == 0);

            ++counterses()[wr].refs;
            ++counterses()[rd].refs;
            ++counterses()[wr].active;
            ++counterses()[rd].active;
        }
        ~Channel() {                                                CSP_LOG(g_verboselog, "%s[%zu:%zu]->~Channel", describe(this), endpts_[0].refcount.load(), endpts_[1].refcount.load());
        }
        csp_writer as_writer() { return reinterpret_cast<csp_writer>(this); }
        csp_reader as_reader() { return reinterpret_cast<csp_reader>((uintptr_t)this | 1); }

        void addref(int endpt) {                                    CSP_LOG(g_verboselog, "%s[%zu:%zu]->addref(%c)", describe(this), endpts_[0].refcount.load() + (endpt == 0), endpts_[1].refcount.load() + (endpt == 1), "wr"[endpt]);
            ++counterses()[endpt].refs;
            endpts_[endpt].refcount.fetch_add(1, std::memory_order_relaxed);
        }
        void release(int endpt) {                                   CSP_LOG(g_verboselog, "%s[%zu:%zu]->release(%c)", describe(this), endpts_[0].refcount.load() - (endpt == 0), endpts_[1].refcount.load() - (endpt == 1), "wr"[endpt]);
            ++counterses()[endpt].derefs;
            if (endpts_[endpt].refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lock(mu_);
                --counterses()[endpt].active;
                auto & ep = endpts_[1 - endpt];
                if (ep.refcount.load(std::memory_order_acquire) > 0) {
                    // Wake waiters via CAS. Don't remove from queues —
                    // woken threads clean up their own registrations.
                    for (auto const & cw : ep.waiters) {            CSP_LOG(g_verboselog, "%s: wake(%s) count=%zu", describe(this), getstatus(cw.thread), ep.waiters.count());
                        uint32_t expected = Microthread::ALT_WAITING;
                        if (cw.thread->alt_state.compare_exchange_strong(expected, Microthread::ALT_CLAIMED)) {
                            int idx = int(cw.chanop - cw.thread->chanops_ + 1);
                            cw.thread->signal_ = -idx;
                            cw.thread->schedule();
                        }
                    }

                    for (auto const & cv : ep.vultures) {
                        uint32_t expected = Microthread::ALT_WAITING;
                        if (cv.thread->alt_state.compare_exchange_strong(expected, Microthread::ALT_CLAIMED)) {
                            int idx = int(cv.chanop - cv.thread->chanops_ + 1);
                            cv.thread->signal_ = -idx;
                            cv.thread->schedule();
                        }
                    }
                    // Don't clear — woken threads clean up their own registrations.
                } else {
                    //delete this;
                }
            }
        }

        explicit operator bool() { return endpts_[wr].refcount.load() > 0 && endpts_[rd].refcount.load() > 0; }

        static int alt(csp_chanop const * chanops, int count, bool nowait) {
            if (count == 1) {
                return prialt(chanops, count, nowait);
            }
            thread_local std::mt19937 rng{std::random_device{}()};
            int offset = std::uniform_int_distribution<int>(0, count - 1)(rng);
            return prialt(chanops, count, nowait, offset);
        }

        static int prialt(csp_chanop const * chanops, int count, bool nowait, int offset = 0) {
            /* */                                                   CSP_LOG(g_verboselog, "prialt%s(..., %d)", nowait ? "<nowait>" : "", count);

            // Collect unique channels, sorted by id for lock ordering.
            Channel* fixed_chans[8];
            std::vector<Channel*> variable_chans;
            Channel** sorted = fixed_chans;
            int n_sorted = 0;
            for (int i = 0; i < count; ++i) {
                if (Channel * ch = chan(chanops[i])) {
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

            auto lock_all = [&]{ for (int i = 0; i < n_sorted; ++i) sorted[i]->mu_.lock(); };
            auto unlock_all = [&]{ for (int i = 0; i < n_sorted; ++i) sorted[i]->mu_.unlock(); };

            lock_all();

            // Phase 1: Scan for ready peer (priority order, rotated by offset).
            bool all_null = true;
            for (int k = 0 ; k < count ; ++k) {
                int i = (offset + k) % count;
                auto const & chop = chanops[i];
                if (Channel * ch = chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter;
                    int endpt = flags & csp_endpt_flag;

                    if (!*ch) {
                        unlock_all();
                        return -(i + 1);
                    }

                    auto & them = ch->endpts_[1 - endpt].waiters;
                    if ((flags & csp_ready_flag)) {
                        for (auto & cw : them) {
                            uint32_t expected = Microthread::ALT_WAITING;
                            if (cw.thread->alt_state.compare_exchange_strong(expected, Microthread::ALT_CLAIMED)) {
                                int idx = int(cw.chanop - cw.thread->chanops_ + 1);
                                cw.thread->signal_ = idx;
                                if (endpt == wr) {                          CSP_LOG(g_verboselog, "PUSH %p[%p] -%p-> %p[%p]", ch, &chop.message, chop.message, cw.thread, &cw.chanop->message);
                                    ;                                       if (g_sequence) { std::cerr << g_self->id_ << " -> " << cw.thread->id_ << " : " << describe(ch) << "\n"; }
                                    if (auto dst = const_cast<void *>(cw.chanop->message)) {
                                        ch->tx_(chop.message, dst);
                                    }
                                    if (Runtime::instance().procs.size() > 1) {
                                        cw.thread->schedule();
                                        unlock_all();
                                    } else {
                                        unlock_all();
                                        cw.thread->run(Status::run);
                                    }
                                } else {                                    CSP_LOG(g_verboselog, "PULL %p[%p] -%p-> %p[%p]", cw.thread, &cw.chanop->message, cw.chanop->message, ch, &chop.message);
                                    ;                                       if (g_sequence) { std::cerr << g_self->id_ << " <- " << cw.thread->id_ << " : " << describe(ch) << "\n"; }
                                    if (auto dst = const_cast<void *>(chop.message)) {
                                        ch->tx_(cw.chanop->message, dst);
                                    }
                                    cw.thread->schedule();
                                    unlock_all();
                                }
                                return i + 1;
                            }
                        }
                    }
                    all_null = false;
                }
            }

            if (all_null || nowait) {                               CSP_LOG(g_verboselog, "prialt() -> %d", 0);
                unlock_all();
                return 0;
            }

            // Phase 2: Register on all channels and sleep.
            g_self->alt_state.store(Microthread::ALT_WAITING, std::memory_order_release);
            for (int i = 0; i < count; ++i) {
                auto const & chop = chanops[i];
                if (Channel * ch = chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter;
                    ch->endpts_[flags & csp_endpt_flag].wait(&chop);
                }
            }

            g_self->chanops_ = chanops;
            g_self->n_chanops_ = count;
            /* */                                                   CSP_LOG(g_sleeplog, "prialt() sleep");
            // Mark suspending_ before unlock_all so that schedule()
            // (called by a waker on another thread) will set
            // wake_pending_ instead of pushing to the global queue.
            // Without this, there is a race: after unlock_all but
            // before do_switch completes, a waker could push us to
            // the global queue and a worker could run us while we
            // haven't finished suspending — double execution.
            g_self->suspending_.store(true, std::memory_order_release);
            unlock_all();
            do_switch(Status::detach);
            g_self->suspending_.store(false, std::memory_order_release);
                                                                    CSP_LOG(g_sleeplog, "prialt() awoken -> %d", g_self->signal_);

            // Phase 3: Woken up — clean up registrations under sorted locks.
            lock_all();
            for (int i = 0; i < g_self->n_chanops_; ++i) {
                auto const & chop = g_self->chanops_[i];
                if (Channel * ch = chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter;
                    ch->endpts_[flags & csp_endpt_flag].remove(&chop, g_self);
                }
            }
            unlock_all();

            g_self->alt_state.store(Microthread::ALT_IDLE, std::memory_order_release);
            auto result = g_self->signal_;
            g_self->chanops_ = nullptr;
            g_self->n_chanops_ = 0;
            return result;
        }

    private:
        using Waiters = detail::RingBuffer<ChanopWaiter>;
        using Vultures = std::unordered_set<ChanopWaiter>;

        // Anticipate channel fusing capability.
        Channel * delegate_ = this;

        size_t id_ = []{ static std::atomic<size_t> last{0}; return ++last; }();
        std::mutex mu_;
        struct EndPoint {
            std::atomic<size_t> refcount{1};
            Waiters waiters;
            Vultures vultures;

            void wait(csp_chanop const * chop) {                   CSP_LOG(g_verboselog, "wait(%s)", describe(chop->waiter));
                auto flags = (uintptr_t)chop->waiter;
                if (flags & csp_ready_flag) {
                    waiters.emplace(chop, g_self);
                } else {
                    vultures.emplace(chop, g_self);
                }
            }

            void remove(csp_chanop const * chop, Microthread * t) {
                                                                    CSP_LOG(g_verboselog, "remove(%s, %s)", describe(chop->waiter), getstatus(t));
                auto flags = (uintptr_t)chop->waiter;
                if (flags & csp_ready_flag) {
                    waiters.remove({chop, t});
                } else {
                    vultures.erase({chop, t});
                }
            }
        } endpts_[2];
        void (* tx_)(void * src, void * dst);

        friend char const * describe(void *);
    };

    // This is horribly inefficient, but, since it is only ever invoked from
    // logging calls (Please keep it that way!), it shouldn't matter.
    char const * describe(void * ch) {
        std::lock_guard<std::mutex> lock(g_chdescrs_mu);
        auto i = g_chdescrs.find(ch);
        if (i == g_chdescrs.end()) {
            char buf[25];
            if (ch) {
                snprintf(buf, sizeof(buf), "▸%lu", ((Channel *)(~(~(uintptr_t)ch | 15)))->id_);
            } else {
                snprintf(buf, sizeof(buf), "▸Ø");
            }
            i = g_chdescrs.emplace(ch, buf).first;
        }
        return i->second.c_str();
    }

}


int csp__internal__channel_count(int endpt) {
    auto & c = counterses()[endpt];
    return c.active - 1; // Exclude skip and global_exception_handler from the reader count.
}

int csp_chan(csp_writer * w, csp_reader * r, void (* tx)(void * src, void * dst)) {
    try {
        auto ch = new Channel{tx};
        *w = ch->as_writer();
        *r = ch->as_reader();
        return int(true);
    } catch (...) { } // TODO: Report the error somehow.
    return int(false);
}

void csp_chdescr(void * ch, char const * descr) {
    static bool enabled = false;
    if (enabled || (enabled = g_chlog || g_lifespan || g_msglog || g_sleeplog)) {
        std::lock_guard<std::mutex> lock(g_chdescrs_mu);
        g_chdescrs[(void*)((uintptr_t)ch & ~14)] = descr;
    }
}

char const * csp__internal__getchdescr(void * ch) {
    return describe(ch);
}

char const * csp__internal__getchflags(void * ch) {
    return descr_flags((uintptr_t)ch);
}

csp_writer csp_writer_addref (csp_writer w) { if (w) chan(w)->addref(wr); return w; }
void        csp_writer_release(csp_writer w) { if (w) chan(w)->release(wr); }
csp_reader csp_reader_addref (csp_reader r) { if (r) chan(r)->addref(rd); return r; }
void        csp_reader_release(csp_reader r) { if (r) chan(r)->release(rd); }

int csp_alt(csp_chanop const * chanops, int count, int nowait) {
    return Channel::alt(chanops, count, bool(nowait));
}

int csp_prialt(csp_chanop const * chanops, int count, int nowait) {
    return Channel::prialt(chanops, count, bool(nowait));
}
