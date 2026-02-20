#pragma once

#include <csp/internal/log.h>

#include <atomic>
#include <cassert>
#include <climits>
#include <exception>
#include <stdint.h>

#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <iterator>


namespace csp::internal {

// Endpoint slot: separately allocated indirection between endpoint handles
// and the channel they target. Enables atomic channel swap (rewiring the
// topology) without touching the handles. Refcount tracks live handles
// (for death detection); channel pointer is swappable.
struct alignas(16) Slot {
    std::atomic<void*> channel{nullptr};   // Channel* (type-erased)
    std::atomic<size_t> refcount{1};       // live endpoint handles

    explicit Slot(void* ch) : channel(ch) {}
};

// Extract Slot* from an endpoint ref (mask off low flag bits).
inline Slot* get_slot(void* ptr) {
    return reinterpret_cast<Slot*>((uintptr_t)ptr & ~15UL);
}

// Opaque channel endpoint handles.
// WriterRef: holds Slot* (16-byte aligned, low bits available for flags).
// ReaderRef: holds (Slot* | 1) — endpoint bit in bit 0.
struct WriterRef { void* ptr = nullptr; explicit operator bool() const { return ptr; } };
struct ReaderRef { void* ptr = nullptr; explicit operator bool() const { return ptr; } };

// Waiter: encoded channel pointer + endpoint + state flags.
// Note: Waiters hold the resolved Channel* (not Slot*), so they reference
// the channel that was current when the waiter was constructed.
struct Waiter { void* ptr = nullptr; };

// State flag constants.
enum : int { dead = 2, ready = dead | 1 };

// Create a waiter for data or death-watch operations.
// Resolves through the endpoint's Slot to get the current Channel*.
inline Waiter wait(WriterRef w) {
    auto* s = get_slot(w.ptr);
    auto* ch = s->channel.load(std::memory_order_acquire);
    return {(void*)((uintptr_t)ch | (ready << 1) | 8)};
}
inline Waiter wait(ReaderRef r) {
    auto* s = get_slot(r.ptr);
    auto* ch = s->channel.load(std::memory_order_acquire);
    return {(void*)((uintptr_t)ch | 1 | (ready << 1) | 8)};
}
inline Waiter wait_dead(WriterRef w) {
    auto* s = get_slot(w.ptr);
    auto* ch = s->channel.load(std::memory_order_acquire);
    return {(void*)((uintptr_t)ch | (dead << 1) | 8)};
}
inline Waiter wait_dead(ReaderRef r) {
    auto* s = get_slot(r.ptr);
    auto* ch = s->channel.load(std::memory_order_acquire);
    return {(void*)((uintptr_t)ch | 1 | (dead << 1) | 8)};
}

// Channel operation descriptor.
struct ChanOp {
    Waiter waiter;
    void * message = nullptr;
    void * slot = nullptr;     // Slot* for re-resolution after channel swap
};

// Two-phase alt/prialt match result.
struct AltMatch {
    int result = 0;
    void * src = nullptr;
    void * dst = nullptr;
    alignas(8) char opaque_[128];
};

// Imp entry function.
using EntryFn = void (*)(void *);

// Imp management.
int spawn(EntryFn entry, void * data);
int run();
void yield();
void descr(char const * fmt, ...);

// Channel creation and refcounting.
bool make_chan(WriterRef * w, ReaderRef * r);
WriterRef writer_addref(WriterRef w);
void writer_release(WriterRef w);
ReaderRef reader_addref(ReaderRef r);
void reader_release(ReaderRef r);

// Channel swap: atomically exchange which channels two endpoint groups target.
void swap_slots(void* slot_a, void* slot_b);

// Channel introspection.
void set_chan_descr(void * ch, char const * descr);

// Two-phase alt/prialt protocol.
void prialt_begin(AltMatch * out, ChanOp const * chanops, int count, int nowait);
void alt_begin(AltMatch * out, ChanOp const * chanops, int count, int nowait);
void alt_end(AltMatch * m);

// Timer.
void sleep_until(int64_t deadline_ns);

// Suspend current imp without placing it in any timer queue.
// Caller must ensure the imp will be rescheduled later.
void suspend();

// Debug/test.
char const * get_descr(void * thr);
char const * get_chan_descr(void * ch);
char const * get_chan_flags(void * ch);
int channel_count(int endpt);

}


#define CSP_DESCR_CHAN__(a) do { CSP_LOG(g_descrlog, "%s = %X:%s", #a, uintptr_t(*(a)) >> 4, uintptr_t(*(a)) & 1 ? "R" : "W"); (a).descr(#a); } while (false)
#define CSP_DESCR_CHAN_(a, ...) CSP_DESCR_CHAN__(a); CSP_DESCR_CHAN_(##__VA_ARGS__);
#define CSP_DESCR_CHAN(a, ...) do { CSP_DESCR_CHAN_(a, ##__VA_ARGS__); } while (false)

#define CSP_DESCR_(F) do { CSP_LOG(g_descrlog, "%s", #F); csp::internal::descr(#F); } while (false)
#define BRAC_DESCR(F, ...) do { CSP_DESCR_(F); CSP_DESCR_CHAN(##__VA_ARGS__); } while (false)


namespace csp
{

namespace detail {

extern Logger g_descrlog;

}

void set_scheduler(std::function<void()> f);
void schedule();

// Yield control so other imps can run. Does nothing outside an imp.
inline void yield() { internal::yield(); }

// Initialize the M:N runtime with the given number of processors (0 = auto).
// If never called, auto-initializes with 1 processor (single-threaded).
void init_runtime(int num_procs = 0);
void shutdown_runtime();

class error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Wrapper for config struct fields that must be explicitly initialized.
// Has no default constructor, so omitting the field in aggregate init is an error.
template <typename T>
struct required {
    T value;
    required() = delete;
    required(T v) : value(std::move(v)) {}
    operator T const&() const { return value; }
    operator T&() { return value; }
};

// Surrogate for empty-message channels.
// - Boost took none.
// - cpplinq squatted on empty.
// - ObjC owns nil.
// - null would get confused with NULL.
extern struct poke_t {
    explicit operator bool() const { return false; }
} poke;

template <typename T> struct chan;

namespace detail {

template <typename T> struct is_chan_op : std::false_type {};

// Compile-time dispatch: call transfer on the op at runtime index idx.
template <int I>
inline void transfer_at(int, void *, void *) {}

template <int I, typename Op, typename... Ops>
inline void transfer_at(int idx, void * src, void * dst, Op && op, Ops &&... ops) {
    if (idx == I) {
        std::decay_t<Op>::transfer(src, dst);
    } else {
        transfer_at<I + 1>(idx, src, dst, std::forward<Ops>(ops)...);
    }
}

}

template <typename T>
class chan_op {
public:
    // Empty/inactive operation (placeholder in vectors).
    chan_op() : active_(false) {}

    // Write operation (copy).
    chan_op(internal::WriterRef w, T const & t)
        : chanop_{internal::wait(w), &buf_, internal::get_slot(w.ptr)}, has_buf_(true)
    {
        new (&buf_) T(t);
    }

    // Write operation (move).
    chan_op(internal::WriterRef w, T && t)
        : chanop_{internal::wait(w), &buf_, internal::get_slot(w.ptr)}, has_buf_(true)
    {
        new (&buf_) T(std::move(t));
    }

    // Read operation.
    template <typename U, typename = std::enable_if_t<std::is_convertible<T, U>::value>>
    chan_op(internal::ReaderRef r, U & dest)
        : chanop_{internal::wait(r), &dest, internal::get_slot(r.ptr)} {}

    // Raw-pointer read (for ring buffer slots and nullptr discard).
    chan_op(internal::ReaderRef r, void * dest)
        : chanop_{internal::wait(r), dest, internal::get_slot(r.ptr)} {}

    // Dead-endpoint operation.
    explicit chan_op(internal::ChanOp op) : chanop_(op) {}

    chan_op(chan_op const &) = delete;
    chan_op(chan_op && o)
        : chanop_(o.chanop_), has_buf_(o.has_buf_), active_(o.active_)
    {
        if (has_buf_) {
            new (&buf_) T(std::move(*reinterpret_cast<T*>(&o.buf_)));
            chanop_.message = &buf_;
            reinterpret_cast<T*>(&o.buf_)->~T();
        }
        o.chanop_ = {{}, nullptr};
        o.has_buf_ = false;
        o.active_ = false;
    }

    ~chan_op() {
        if (active_) {
            internal::AltMatch m;
            internal::prialt_begin(&m, &chanop_, 1, false);
            if (m.src && m.dst)
                *static_cast<T *>(m.dst) = std::move(*static_cast<T *>(m.src));
            internal::alt_end(&m);
        }
        if (has_buf_) reinterpret_cast<T*>(&buf_)->~T();
    }

    chan_op & operator=(chan_op const &) = delete;
    chan_op & operator=(chan_op && o) {
        if (has_buf_) {
            reinterpret_cast<T*>(&buf_)->~T();
            has_buf_ = false;
        }
        chanop_ = o.chanop_;
        has_buf_ = o.has_buf_;
        active_ = o.active_;
        if (has_buf_) {
            new (&buf_) T(std::move(*reinterpret_cast<T*>(&o.buf_)));
            chanop_.message = &buf_;
            reinterpret_cast<T*>(&o.buf_)->~T();
        }
        o.chanop_ = {{}, nullptr};
        o.has_buf_ = false;
        o.active_ = false;
        return *this;
    }

    explicit operator bool() const {
        active_ = false;
        internal::AltMatch m;
        internal::prialt_begin(&m, &chanop_, 1, false);
        if (m.src && m.dst)
            *static_cast<T *>(m.dst) = std::move(*static_cast<T *>(m.src));
        internal::alt_end(&m);
        return m.result >= 0;
    }

    static void transfer(void * src, void * dst) {
        *static_cast<T *>(dst) = std::move(*static_cast<T *>(src));
    }

    void disarm() const { active_ = false; }
    internal::ChanOp chanop() const { return chanop_; }

private:
    internal::ChanOp chanop_ = {{}, nullptr};
    mutable std::aligned_storage_t<sizeof(T), alignof(T)> buf_;
    bool has_buf_ = false;
    mutable bool active_ = true;
};

// Non-blocking guard for alt/prialt.  Fires (returning csp::none) when no
// other channel operation is ready, instead of blocking.
//
//   switch (prialt(r >> n, w << val, csp::none)) {
//   case 0:         /* read matched  */ break;
//   case 1:         /* write matched */ break;
//   case csp::none: /* nothing ready */ break;
//   }
struct none_t {
    static constexpr int value = INT_MIN;
    constexpr operator int() const { return value; }
    internal::ChanOp chanop() const { return {{}, nullptr}; }
    static void transfer(void*, void*) {}
    void disarm() const {}
};
inline constexpr none_t none{};

namespace detail {
template <typename T> struct is_chan_op<chan_op<T>> : std::true_type {};
template <> struct is_chan_op<none_t> : std::true_type {};
}


template <typename T = poke_t>
class writer {
public:
    static writer dead();

    writer() = default;
    writer(writer const &) = delete;
    writer(writer && w) : w_(w.w_) { w.w_ = {}; }
    ~writer() { // TLA:ChannelLifecycle.WaiterReleaseRef
        if (w_) {
            internal::writer_release(w_);
        }
    }

    writer& operator=(writer const &) = delete;
    writer<T>& operator=(writer && w) {
        if (w_) internal::writer_release(w_);
        w_ = w.w_;
        w.w_ = {};
        return *this;
    }
    bool operator==(const writer& w) const { return w_.ptr == w.w_.ptr; }
    bool operator!=(const writer& w) const { return !(*this == w); }
    explicit operator bool() const { return bool(w_); }

    void descr(const char* d) const { internal::set_chan_descr(w_.ptr, d); }

    chan_op<T> operator<<(T const & t) const { return {w_, t}; }
    chan_op<T> operator<<(T && t) const { return {w_, std::move(t)}; }

    chan_op<T> operator~() const {
        return chan_op<T>(internal::ChanOp{internal::wait_dead(w_), nullptr, internal::get_slot(w_.ptr)});
    }

    writer copy() const {
        writer c;
        c.w_ = w_;
        if (c.w_) internal::writer_addref(c.w_);
        return c;
    }

    internal::WriterRef internal_writer() const { return w_; }

private:
    mutable internal::WriterRef w_;

    void assign(internal::WriterRef w) { w_ = w; }

    friend struct chan<T>;
};

namespace { Logger g_reader_log("reader"); }

template <typename T = poke_t>
class reader {
public:
    static reader dead();

    reader() = default;
    reader(reader const &) = delete;
    reader(reader && r) : r_(r.r_) { r.r_ = {}; }
    ~reader() { // TLA:ChannelLifecycle.WaiterReleaseRef
        if (r_) {
            internal::reader_release(r_);
        }
    }

    reader& operator=(reader const &) = delete;
    reader& operator=(reader && r) {
        if (r_) internal::reader_release(r_);
        r_ = r.r_;
        r.r_ = {};
        return *this;
    }
    bool operator==(const reader& r) const { return r_.ptr == r.r_.ptr; }
    bool operator!=(const reader& r) const { return !(*this == r); }
    explicit operator bool() const  { return bool(r_); }

    void descr(const char* d) { internal::set_chan_descr(r_.ptr, d); }

    template <typename U>
    std::enable_if_t<std::is_convertible<T, U>::value, chan_op<T>>
    operator>>(U & u) const {
        return {r_, u};
    }
    chan_op<T> operator>>(void * dest) const {
        return {r_, dest};
    }

    // Connect two channels directly.
    template <typename U>
    auto stream_to(writer<U> out) const {
        return [in = this->copy(), out = std::move(out)] {
            for (T t; prialt(~out, in >> t) >= 0 && out << t;) { }
        };
    }

    // Read and return one message.
    T read() const {
        T t;
        if (!(*this >> t)) {
            throw error("reader exhausted");
        }
        return t;
    }

    // Read exactly one value and assert the reader produces no more.
    T single() const {
        T t;
        assert(static_cast<bool>(*this >> t) && "single() called on exhausted reader");
        T discard;
        assert(!static_cast<bool>(*this >> discard) && "single() reader produced more than one value");
        return t;
    }

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T const *;
        using reference = T const &;

        iterator() : source_(nullptr) {}
        iterator(reader<T> const & source) : source_(source ? &source : nullptr) {
            if (source_) advance();
        }

        reference operator*() const { return t_; }
        pointer operator->() const { return &t_; }

        iterator & operator++() { advance(); return *this; }
        iterator operator++(int) { auto tmp = *this; advance(); return tmp; }

        friend bool operator==(iterator const & a, iterator const & b) { return a.source_ == b.source_; }
        friend bool operator!=(iterator const & a, iterator const & b) { return a.source_ != b.source_; }

    private:
        reader<T> const * source_;
        T t_;

        void advance() {
            if (!(*source_ >> t_)) {
                source_ = nullptr;
            }
        }
    };

    iterator begin() const { return {*this}; }
    iterator end() const { return {}; }

    chan_op<T> operator~() const {
        return chan_op<T>(internal::ChanOp{internal::wait_dead(r_), nullptr, internal::get_slot(r_.ptr)});
    }

    reader copy() const {
        reader c;
        c.r_ = r_;
        if (c.r_) internal::reader_addref(c.r_);
        return c;
    }

    internal::ReaderRef internal_reader() const { return r_; }

private:
    mutable internal::ReaderRef r_;

    void assign(internal::ReaderRef r) { r_ = r; }

    friend struct chan<T>;
};

template <typename T = poke_t>
struct chan {
    writer<T> w;
    reader<T> r;

    chan() {
        internal::WriterRef cw;
        internal::ReaderRef cr;
        if (!internal::make_chan(&cw, &cr)) {
            throw error("channel creation failed");
        }
        w.assign(cw);
        r.assign(cr);
    }

    chan(writer<T> w, reader<T> r) : w(std::move(w)), r(std::move(r)) {}

    chan(chan const &) = delete;
    chan(chan &&) = default;
    chan & operator=(chan const &) = delete;
    chan & operator=(chan &&) = default;

    void release() {
        w = {};
        r = {};
    }
};

template <typename T>
reader<T> reader<T>::dead() {
    return std::move(chan<T>().r);
}

template <typename T>
writer<T> writer<T>::dead() {
    return std::move(chan<T>().w);
}


template <typename T>
void make_channel(writer<T> & w, reader<T> & r) {
    auto [cw, cr] = chan<T>{};
    w = std::move(cw);
    r = std::move(cr);
}

// Atomically swap which channels two writer (or reader) groups target.
// All holders of copies of a's endpoint are transparently redirected to b's
// channel, and vice versa.
template <typename T>
void swap(writer<T>& a, writer<T>& b) {
    internal::swap_slots(
        internal::get_slot(a.internal_writer().ptr),
        internal::get_slot(b.internal_writer().ptr));
}

template <typename T>
void swap(reader<T>& a, reader<T>& b) {
    internal::swap_slots(
        internal::get_slot(a.internal_reader().ptr),
        internal::get_slot(b.internal_reader().ptr));
}

// 4-arg swap: exchange which channels two (writer, reader) pairs target.
// The middle two parameters (r1, w2) are by value: when both are empty,
// a temporary channel is created (fuse mode); otherwise their destruction
// on return triggers death on the channels they were swapped onto.
//
//   Fuse:  swap(a.w, {}, {}, b.r)
//   Split: swap(w, std::move(a.r), std::move(b.w), r)
//
// TODO: Improve behavior when fusing/splitting channels with active waiters.
// Currently, waiters blocked in prialt on a channel whose slot is redirected
// remain registered on the old channel until something wakes them (typically
// endpoint death when the old channel becomes orphaned).  Ideally, swap_slots
// would deregister and re-register affected waiters on the new channel so that
// fuse/split can be applied mid-flight without a re-resolution stall.  The two
// sequential swaps also create a brief intermediate state visible under M:N
// concurrency; an atomic multi-swap could eliminate this window.
template <typename T>
void swap(writer<T>& w1, reader<T> r1, writer<T> w2, reader<T>& r2) {
    if (!r1 && !w2) {
        chan<T> temp;
        swap(w1, temp.w);
        swap(temp.r, r2);
        return;
    }
    swap(w1, w2);
    swap(r1, r2);
}

// Fuse two endpoints: redirect w onto r's channel.
// Orphaned sides on the original channels see death.
template <typename T>
void fuse(writer<T>& w, reader<T>& r) {
    swap(w, {}, {}, r);
}

// Tap: splice a pass-through observer into the channel between w and r.
// Returns a tap_handle whose `output` reader receives a copy of every value
// flowing from w to r.  Destroying the handle fuses w and r back together,
// removing the forwarding imp from the data path.
template <typename T>
struct tap_handle {
    reader<T> output;   // tap stream — reads a copy of each forwarded value

    tap_handle() = default;
    tap_handle(tap_handle const &) = delete;
    tap_handle(tap_handle &&) = default;
    tap_handle & operator=(tap_handle const &) = delete;
    tap_handle & operator=(tap_handle &&) = default;

    ~tap_handle() {
        if (!w_copy_) return;       // moved-from or default-constructed
        output = {};                // kill tap reader
        fuse(w_copy_, r_copy_);     // redirect w,r back together
    }

private:
    writer<T> w_copy_;  // shares slot with caller's w
    reader<T> r_copy_;  // shares slot with caller's r

    template <typename U>
    friend tap_handle<U> tap(writer<U>& w, reader<U>& r);
};

template <typename T>
tap_handle<T> tap(writer<T>& w, reader<T>& r) {
    chan<T> a, b, tap_ch;

    // Split: w → [B], [A] → r.  Original channel dies.
    swap(w, std::move(a.r), std::move(b.w), r);

    // Forwarder: reads from B (what w writes to), writes to A (what r reads
    // from) and to tap_ch.  When tap_ch's reader dies, stops tapping but
    // continues forwarding.  When B or A dies, exits.
    spawn([br = std::move(b.r), aw = std::move(a.w),
           tw = std::move(tap_ch.w)]() mutable {
        for (T t; prialt(~aw, br >> t) >= 0;) {
            if (tw && !(tw << t)) tw = {};  // tap reader gone — stop tapping
            if (!(aw << t)) break;          // downstream dead — exit
        }
    });

    tap_handle<T> h;
    h.output = std::move(tap_ch.r);
    h.w_copy_ = w.copy();
    h.r_copy_ = r.copy();
    return h;
}

// Backward compatibility.
template <typename T>
void channel_swap(writer<T>& a, writer<T>& b) { swap(a, b); }
template <typename T>
void channel_swap(reader<T>& a, reader<T>& b) { swap(a, b); }

// Make a channel for a writer&, returning the matching reader.
template <typename T>
reader<T> operator--(writer<T> & w) {
    if (w) {
        throw error("writer already attached channel");
    }
    auto [cw, cr] = chan<T>{};
    w = std::move(cw);
    return std::move(cr);
}

// Make a channel for a reader&, returning the matching writer.
template <typename T>
writer<T> operator++(reader<T> & r) {
    if (r) {
        throw error("reader already attached to channel");
    }
    auto [cw, cr] = chan<T>{};
    r = std::move(cr);
    return std::move(cw);
}

extern writer<std::exception_ptr> global_exception_handler;

struct ClientSide { };
struct ServerSide { };

namespace detail {

template <typename Side, typename T> struct IncomingEndPoint;
template <typename Side, typename T> struct OutgoingEndPoint;

template <typename T> struct IncomingEndPoint<ClientSide, T> { using type = writer<T>; };
template <typename T> struct IncomingEndPoint<ServerSide, T> { using type = reader<T>; };

template <typename T> struct OutgoingEndPoint<ClientSide, T> { using type = reader<T>; };
template <typename T> struct OutgoingEndPoint<ServerSide, T> { using type = writer<T>; };

}

template <typename Side, typename T = poke_t> using incoming = typename detail::template IncomingEndPoint<Side, T>::type;
template <typename Side, typename T = poke_t> using outgoing = typename detail::template OutgoingEndPoint<Side, T>::type;

namespace detail {

template <typename F>
struct spawn_data {
    F f;
    writer<std::exception_ptr> w;
};

template <typename F>
inline void spawn_entry(void * data) {
    std::unique_ptr<spawn_data<F>> sd{static_cast<spawn_data<F> *>(data)};
    try {
        auto f = std::move(sd->f);
        f();
    } catch (...) {
        auto ex = std::current_exception();
        if (!(sd->w << ex) && !(global_exception_handler << ex)) {
            std::terminate();
        }
    }
};

}

template <typename F>
reader<std::exception_ptr> spawn(F && f) {
    reader<std::exception_ptr> r;
    auto sd = new detail::spawn_data<F>{std::move(f), ++r};
    if (!internal::spawn(detail::spawn_entry<F>, sd)) {
        throw error("spawn failed");
    }
    return r;
}

inline void join(reader<std::exception_ptr> const & r) {
    std::exception_ptr ep;
    if (r >> ep) {
        std::rethrow_exception(ep);
    }
}

template <typename T, typename F>
writer<T> spawn_consumer(F f) {
    writer<T> w;
    spawn([f = std::move(f), r = --w]() mutable {
        f(std::move(r));
    });
    return w;
}

template <typename T, typename F>
reader<T> spawn_producer(F && f) {
    reader<T> r;
    spawn([f = std::move(f), w = ++r]() mutable {
        f(std::move(w));
    });
    return r;
}

template <typename T, typename F>
chan<T> spawn_filter(F && f) {
    auto [in_w, in_r] = chan<T>{};
    auto [out_w, out_r] = chan<T>{};
    spawn([f = std::move(f), r = std::move(in_r), w = std::move(out_w)]() mutable {
        f(std::move(r), std::move(w));
    });
    return {std::move(in_w), std::move(out_r)};
}

// Range over a producer µthread; propagate exceptions therefrom.
template <typename T>
class range {
public:
    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T const *;
        using reference = T const &;

        iterator(range<T> const * source = {}) : source_(source) {
            if (source_ && source_->r_) {
                advance();
            }
        }

        reference operator*() const { return t_; }
        pointer operator->() const { return &t_; }

        iterator & operator++() { advance(); return *this; }
        iterator operator++(int) { auto tmp = *this; advance(); return tmp; }

        friend bool operator==(iterator const & a, iterator const & b) { return a.source_ == b.source_; }
        friend bool operator!=(iterator const & a, iterator const & b) { return a.source_ != b.source_; }

    private:
        range<T> const * source_;
        T t_;

        void advance() {
            if (!(source_->r_ >> t_)) {
                join(source_->ex_);
                source_ = {};
            }
        }
    };

    range(reader<T> r, reader<std::exception_ptr> ex) : r_(std::move(r)), ex_(std::move(ex)) { }

    iterator begin() const { return {this}; }
    iterator end() const { return {}; }

    reader<T> const & source() const { return r_; }
    reader<std::exception_ptr> const & except() const { return ex_; }

private:
    reader<T> r_;
    reader<std::exception_ptr> ex_;
};

template <typename T, typename F>
range<T> spawn_range(F f) {
    reader<T> r;
    auto ex = spawn([f = std::move(f), w = ++r]() mutable {
        f(std::move(w));
    });
    return {std::move(r), std::move(ex)};
}

namespace detail {

using alt_begin_f = void(internal::AltMatch *, internal::ChanOp const *, int, int);

// Typed variadic alt: compile-time dispatch, no function pointers.
template <alt_begin_f * begin_f, typename... Ops>
inline
std::enable_if_t<(is_chan_op<std::decay_t<Ops>>::value && ...), int>
typed_alt(Ops &&... ops) {
    constexpr bool has_none = (std::is_same_v<std::decay_t<Ops>, none_t> || ...);
    constexpr size_t N = sizeof...(Ops);
    internal::ChanOp chanops[N] = {ops.chanop()...};
    internal::AltMatch m;
    begin_f(&m, chanops, N, has_none);
    if (m.src && m.dst) {
        int idx = (m.result >= 0 ? m.result : ~m.result);
        transfer_at<0>(idx, m.src, m.dst, ops...);
    }
    internal::alt_end(&m);
    (ops.disarm(), ...);
    return m.result;
}

// Typed vector alt: all operations share type T, transfer is inline.
//
// Dynamic-count alt where every operation is on the same channel
// type T.  This is not a limitation in practice: dynamic-count alt
// arises when fan-out/fan-in targets a runtime-determined set of
// channels, and those channels are always the same type.  When
// different channel types appear in a single alt, the count is
// known at compile time and the variadic overload handles it with
// per-index type dispatch.
template <alt_begin_f * begin_f, typename T>
inline
int typed_alt_vec(std::vector<chan_op<T>> const & ops) {
    std::vector<internal::ChanOp> chanops;
    chanops.reserve(ops.size());
    for (auto & op : ops) chanops.push_back(op.chanop());
    internal::AltMatch m;
    begin_f(&m, chanops.data(), (int)chanops.size(), 0);
    if (m.src && m.dst)
        chan_op<T>::transfer(m.src, m.dst);
    internal::alt_end(&m);
    for (auto & op : ops) op.disarm();
    return m.result;
}

// Typed vector alt with nowait (for none support).
template <alt_begin_f * begin_f, typename T>
inline
int typed_alt_vec_none(std::vector<chan_op<T>> const & ops) {
    std::vector<internal::ChanOp> chanops;
    chanops.reserve(ops.size());
    for (auto & op : ops) chanops.push_back(op.chanop());
    internal::AltMatch m;
    begin_f(&m, chanops.data(), (int)chanops.size(), 1);
    if (m.src && m.dst)
        chan_op<T>::transfer(m.src, m.dst);
    internal::alt_end(&m);
    for (auto & op : ops) op.disarm();
    return m.result;
}

}

// --- variadic overloads (compile-time type dispatch) ---

template <typename... Ops>
inline
std::enable_if_t<(detail::is_chan_op<std::decay_t<Ops>>::value && ...), int>
alt(Ops &&... ops) {
    return detail::typed_alt<&internal::alt_begin>(std::forward<Ops>(ops)...);
}

template <typename... Ops>
inline
std::enable_if_t<(detail::is_chan_op<std::decay_t<Ops>>::value && ...), int>
prialt(Ops &&... ops) {
    return detail::typed_alt<&internal::prialt_begin>(std::forward<Ops>(ops)...);
}

// --- vector overloads (single type T, inline transfer) ---

template <typename T>
inline
int alt(std::vector<chan_op<T>> const & ops) {
    return detail::typed_alt_vec<&internal::alt_begin>(ops);
}

template <typename T>
inline
int prialt(std::vector<chan_op<T>> const & ops) {
    return detail::typed_alt_vec<&internal::prialt_begin>(ops);
}

// --- vector+none overloads ---

template <typename T>
inline
int alt(std::vector<chan_op<T>> const & ops, none_t) {
    return detail::typed_alt_vec_none<&internal::alt_begin>(ops);
}

template <typename T>
inline
int prialt(std::vector<chan_op<T>> const & ops, none_t) {
    return detail::typed_alt_vec_none<&internal::prialt_begin>(ops);
}

// Dead channel to assist non-blocking waits.
extern reader<> const skip;

}

