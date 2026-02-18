#pragma once

#include <csp/internal/mt_log.h>

#include <exception>
#include <stdint.h>

#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <iterator>


namespace csp::internal {

// Opaque channel endpoint handles.
// WriterRef: holds Channel* (16-byte aligned, low bits available for flags).
// ReaderRef: holds (Channel* | 1) — endpoint bit in bit 0.
struct WriterRef { void* ptr = nullptr; explicit operator bool() const { return ptr; } };
struct ReaderRef { void* ptr = nullptr; explicit operator bool() const { return ptr; } };

// Waiter: encoded channel pointer + endpoint + state flags.
struct Waiter { void* ptr = nullptr; };

// State flag constants.
enum : int { dead = 2, ready = dead | 1 };

// Create a waiter for data or death-watch operations.
inline Waiter wait(WriterRef w)      { return {(void*)((uintptr_t)w.ptr | (ready << 1) | 8)}; }
inline Waiter wait(ReaderRef r)      { return {(void*)((uintptr_t)r.ptr | (ready << 1) | 8)}; }
inline Waiter wait_dead(WriterRef w) { return {(void*)((uintptr_t)w.ptr | (dead << 1) | 8)}; }
inline Waiter wait_dead(ReaderRef r) { return {(void*)((uintptr_t)r.ptr | (dead << 1) | 8)}; }

// Channel operation descriptor.
struct ChanOp {
    Waiter waiter;
    void * message = nullptr;
};

// Two-phase alt/prialt match result.
struct AltMatch {
    int result = 0;
    void * src = nullptr;
    void * dst = nullptr;
    alignas(8) char opaque_[128];
};

// Microthread entry function.
using EntryFn = void (*)(void *);

// Microthread management.
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

// Channel introspection.
void set_chan_descr(void * ch, char const * descr);

// Two-phase alt/prialt protocol.
void prialt_begin(AltMatch * out, ChanOp const * chanops, int count, int nowait);
void alt_begin(AltMatch * out, ChanOp const * chanops, int count, int nowait);
void alt_end(AltMatch * m);

// Timer.
void sleep_until(int64_t deadline_ns);

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

// Yield control so other microthreads can run. Does nothing outside a
// microthread.
inline void yield() { internal::yield(); }

// Initialize the M:N runtime with the given number of processors (0 = auto).
// If never called, auto-initializes with 1 processor (single-threaded).
void init_runtime(int num_procs = 0);
void shutdown_runtime();

class microthread_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
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
        : chanop_{internal::wait(w), &buf_}, has_buf_(true)
    {
        new (&buf_) T(t);
    }

    // Write operation (move).
    chan_op(internal::WriterRef w, T && t)
        : chanop_{internal::wait(w), &buf_}, has_buf_(true)
    {
        new (&buf_) T(std::move(t));
    }

    // Read operation.
    template <typename U, typename = std::enable_if_t<std::is_convertible<T, U>::value>>
    chan_op(internal::ReaderRef r, U & dest)
        : chanop_{internal::wait(r), &dest} {}

    // Raw-pointer read (for ring buffer slots and nullptr discard).
    chan_op(internal::ReaderRef r, void * dest)
        : chanop_{internal::wait(r), dest} {}

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

namespace detail {
template <typename T> struct is_chan_op<chan_op<T>> : std::true_type {};
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
    void swap(writer& w) {
        auto tmp = w_;
        w_ = w.w_;
        w.w_ = tmp;
    }

    bool operator==(const writer& w) const { return w_.ptr == w.w_.ptr; }
    bool operator!=(const writer& w) const { return !(*this == w); }
    explicit operator bool() const { return bool(w_); }

    void descr(const char* d) const { internal::set_chan_descr(w_.ptr, d); }

    chan_op<T> operator<<(T const & t) const { return {w_, t}; }
    chan_op<T> operator<<(T && t) const { return {w_, std::move(t)}; }

    chan_op<T> operator~() const {
        return chan_op<T>(internal::ChanOp{internal::wait_dead(w_), nullptr});
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
    void swap(reader& i) {
        auto tmp = r_;
        r_ = i.r_;
        i.r_ = tmp;
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
            throw microthread_error("reader exhausted");
        }
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
        return chan_op<T>(internal::ChanOp{internal::wait_dead(r_), nullptr});
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
            throw microthread_error("channel creation failed");
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

// Make a channel for a writer&, returning the matching reader.
template <typename T>
reader<T> operator--(writer<T> & w) {
    if (w) {
        throw microthread_error("writer already attached channel");
    }
    auto [cw, cr] = chan<T>{};
    w = std::move(cw);
    return std::move(cr);
}

// Make a channel for a reader&, returning the matching writer.
template <typename T>
writer<T> operator++(reader<T> & r) {
    if (r) {
        throw microthread_error("reader already attached to channel");
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
        throw microthread_error("spawn failed");
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
    constexpr size_t N = sizeof...(Ops);
    internal::ChanOp chanops[N] = {ops.chanop()...};
    internal::AltMatch m;
    begin_f(&m, chanops, N, false);
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

// Dead channel to assist non-blocking waits.
extern reader<> const skip;

}

namespace std {

template <typename T>
void swap(csp::writer<T>& a, csp::writer<T>& b) {
    a.swap(b);
}

template <typename T>
void swap(csp::reader<T>& a, csp::reader<T>& b) {
    a.swap(b);
}

}
