#include <csp/internal/microthread_internal.h>
#include <csp/ringbuffer.h>

#include <boost/range/iterator_range_core.hpp>

#include <algorithm>
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
#include <numeric>
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

static std::map<void *, std::string> g_chdescrs;

struct Counters {
    int refs = 0;
    int derefs = 0;
    int active = 0;
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
        Channel(void (* tx)(void * src, void * dst)) : tx_(tx) {     CSP_LOG(g_verboselog, "new (%s[%d:%d]) Channel", describe(this), endpts_[0].refcount, endpts_[1].refcount);
            // Must be 16-byte aligned.
            assert(((uintptr_t)this % 16) == 0);

            ++counterses()[wr].refs;
            ++counterses()[rd].refs;
            ++counterses()[wr].active;
            ++counterses()[rd].active;
        }
        ~Channel() {                                                CSP_LOG(g_verboselog, "%s[%d:%d]->~Channel", describe(this), endpts_[0].refcount, endpts_[1].refcount);
        }
        csp_writer as_writer() { return reinterpret_cast<csp_writer>(this); }
        csp_reader as_reader() { return reinterpret_cast<csp_reader>((uintptr_t)this | 1); }

        void addref(int endpt) {                                    CSP_LOG(g_verboselog, "%s[%d:%d]->addref(%c)", describe(this), endpts_[0].refcount + (endpt == 0), endpts_[1].refcount + (endpt == 1), "wr"[endpt]);
            ++counterses()[endpt].refs;
            ++endpts_[endpt].refcount;
        }
        void release(int endpt) {                                   CSP_LOG(g_verboselog, "%s[%d:%d]->release(%c)", describe(this), endpts_[0].refcount - (endpt == 0), endpts_[1].refcount - (endpt == 1), "wr"[endpt]);
            ++counterses()[endpt].derefs;
            if (!--endpts_[endpt].refcount) {
                --counterses()[endpt].active;
                auto & ep = endpts_[1 - endpt];
                if (ep.refcount) {
                    auto & w = ep.waiters;
                    while (!w.empty()) {
                        auto const & cw = w.front();                CSP_LOG(g_verboselog, "%s: unwait(%s) count=%d", describe(this), getstatus(cw.thread), w.count());
                        cw.thread->schedule();
                        cw.thread->unwait(cw.chanop, false);
                    }

                    auto & v = ep.vultures;
                    while (!v.empty()) {
                        auto const & cv = *begin(v);
                        int i = int(cv.chanop - cv.thread->chanops_ + 1);
                        cv.thread->signal_ = -i;
                        cv.thread->schedule();
                        cv.thread->unwait(cv.chanop, false);
                    }
                    ep.vultures.clear();
                } else {
                    //delete this;
                }
            }
        }

        explicit operator bool() { return endpts_[wr].refcount && endpts_[rd].refcount; }

        // TODO: Make this less ugly and faster.
        static int alt(csp_chanop const * chanops, int count, bool nowait) {
            if (count == 1) {
                return prialt(chanops, count, nowait);
            }
            csp_chanop fixed[8];
            std::vector<csp_chanop> variable;
            csp_chanop * buffer = fixed;
            if (count <= 8) {
                std::copy(chanops, chanops + count, buffer);
            } else {
                variable = {chanops, chanops + count};
                buffer = &variable.front();
            }

            std::random_device rdev;
            std::mt19937 rng(rdev());
            std::shuffle(buffer, buffer + count, rng);

            int i = prialt(buffer, count, nowait);
            if (i > 0) {
                auto signalled = chan(buffer[i - 1]);
                for (int j = 0; j < count; ++j) {
                    if (chan(chanops[j]) == signalled) {            CSP_LOG(g_verboselog, "alt() -> %d", j + 1);
                        if (((uintptr_t)chanops[j].waiter & csp_endpt_flag) == rd) {
                            const_cast<void * &>(chanops[j].message) = buffer[i - 1].message;
                        }
                        return j + 1;
                    }
                }
            } else if (i < 0) {
                auto signalled = chan(buffer[-i - 1]);
                for (int j = 0; j < count; ++j) {
                    if (chan(chanops[j]) == signalled) {            CSP_LOG(g_verboselog, "alt() -> %d", -(j + 1));
                        return -(j + 1);
                    }
                }
            }
            assert(!i);
            return 0;
        }

        static int prialt(csp_chanop const * chanops, int count, bool nowait) {
            /* */                                                   CSP_LOG(g_verboselog, "prialt%s(..., %d)", nowait ? "<nowait>" : "", count);
            int all_null = true;
            for (int i = 0 ; i < count ; ++i) {
                auto const & chop = chanops[i];
                if (Channel * ch = chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter;
                    int endpt = flags & csp_endpt_flag;

                    if (!*ch) {
                        return -(i + 1);
                    }

                    auto & them = ch->endpts_[1 - endpt].waiters;
                    if ((flags & csp_ready_flag) && !them.empty()) {
                        auto cw = them.front();
                        cw.thread->unwait(cw.chanop, true);
                        if (endpt == wr) {                          CSP_LOG(g_verboselog, "PUSH %p[%p] -%p-> %p[%p]", ch, &chop.message, chop.message, cw.thread, &cw.chanop->message);
                            ;                                       if (g_sequence) { std::cerr << g_self->id_ << " -> " << cw.thread->id_ << " : " << describe(ch) << "\n"; }
                            if (auto dst = const_cast<void *>(cw.chanop->message)) {
                                ch->tx_(chop.message, dst);
                            }
                            cw.thread->run(Status::run);
                        } else {                                    CSP_LOG(g_verboselog, "PULL %p[%p] -%p-> %p[%p]", cw.thread, &cw.chanop->message, cw.chanop->message, ch, &chop.message);
                            ;                                       if (g_sequence) { std::cerr << g_self->id_ << " <- " << cw.thread->id_ << " : " << describe(ch) << "\n"; }
                            if (auto dst = const_cast<void *>(chop.message)) {
                                ch->tx_(cw.chanop->message, dst);
                            }
                            cw.thread->schedule(); // Reader needs time to grok.
                        }
                        return i + 1;
                    }
                    all_null = false;
                }
            }

            if (all_null || nowait) {                               CSP_LOG(g_verboselog, "prialt() -> %d", 0);
                return 0;
            }

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
            do_switch(Status::detach);                              CSP_LOG(g_sleeplog, "prialt() awoken -> %d", g_self->signal_);

            assert(!g_self->chanops_);

            return g_self->signal_;
        }

    private:
        using Waiters = detail::RingBuffer<ChanopWaiter>;
        using Vultures = std::unordered_set<ChanopWaiter>;

        // Anticipate channel fusing capability.
        Channel * delegate_ = this;

        size_t id_ = []{ static size_t last = 0; return ++last; }();
        struct EndPoint {
            size_t refcount = 1;
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

        friend struct detail::Microthread;
        friend char const * describe(void *);
    };

    // This is horribly inefficient, but, since it is only ever invoked from
    // logging calls (Please keep it that way!), it shouldn't matter.
    char const * describe(void * ch) {
        auto i = g_chdescrs.find(ch);
        if (i == g_chdescrs.end()) {
            char buf[25];
            if (ch) {
                sprintf(buf, "▸%lu", ((Channel *)(~(~(uintptr_t)ch | 15)))->id_);
            } else {
                sprintf(buf, "▸Ø");
            }
            i = g_chdescrs.emplace(ch, buf).first;
        }
        return i->second.c_str();
    }

}

namespace csp {

    namespace detail {

        void Microthread::unwait(csp_chanop const * signalled, bool ready) {
            ;                                                       CSP_LOG(g_verboselog, "unwait(%s, %s)", describe(signalled->waiter), ready ? "true" : "false");
            static_assert(offsetof(Channel,delegate_) == 0, "delegate_ must be at the start for chan() to work");

            assert(chanops_);

            for (int i = 0; i < n_chanops_; ++i) {
                auto const & chop = chanops_[i];
                if (Channel * ch = chan(chop)) {
                    auto flags = (uintptr_t)chop.waiter;
                    ch->endpts_[flags & csp_endpt_flag].remove(&chop, this);
                }
            }
            int i = int(signalled - chanops_ + 1);
            signal_ = ready ? i : -i;
            chanops_ = nullptr;
            n_chanops_ = 0;
        }

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
