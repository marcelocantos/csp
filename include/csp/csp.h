#pragma once

#define CSP_VERSION "0.5.0"
#define CSP_VERSION_MAJOR 0
#define CSP_VERSION_MINOR 5
#define CSP_VERSION_PATCH 0

#include <csp/internal/log.h>
#include <csp/ringbuffer.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <climits>
#include <exception>
#include <memory>
#include <stdint.h>

#include <functional>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>


namespace csp::internal {

// Endpoint slot: separately allocated indirection between endpoint handles
// and the channel they target. Enables atomic channel swap (rewiring the
// topology) without touching the handles. Refcount tracks live handles
// (for death detection); channel pointer is swappable.
struct alignas(16) Slot {
    std::atomic<void*> channel{nullptr};   // Channel* (type-erased)
    std::atomic<size_t> refcount{1};       // strong refs (death detection)
    std::atomic<size_t> mem_refcount{1};   // memory refs: 1 from channel + N from weak refs
    std::atomic_flag spin_ = ATOMIC_FLAG_INIT;  // serializes re-resolution with swap_slots

    explicit Slot(void* ch) : channel(ch) {}

    void lock() { while (spin_.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { spin_.clear(std::memory_order_release); }

    // Release one memory ref. Deletes the slot when the last ref is released.
    void mem_release() {
        if (mem_refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

// Extract Slot* from an endpoint ref (mask off low flag bits).
inline Slot* get_slot(void* ptr) {
    return reinterpret_cast<Slot*>((uintptr_t)ptr & ~uintptr_t{15});
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

// Weak endpoint references: keep slot memory alive without participating
// in death detection (refcount). Increment mem_refcount instead.
// try_upgrade atomically increments refcount (CAS from N>0 to N+1),
// returning true on success. On failure the endpoint is dead.
void writer_weak_addref(WriterRef w);
void writer_weak_release(WriterRef w);
void reader_weak_addref(ReaderRef r);
void reader_weak_release(ReaderRef r);
bool try_upgrade_weak_writer(WriterRef w);
bool try_upgrade_weak_reader(ReaderRef r);

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

using byte = uint8_t;
using bytes = std::vector<byte>;

namespace detail {

extern Logger g_descrlog;

}

void set_scheduler(std::function<void()> f);
void reset_scheduler();
void schedule();

// Yield control so other imps can run. Does nothing outside an imp.
inline void yield() { internal::yield(); }

// Set the target number of processors (OS threads). 0 = auto (hardware
// concurrency). The system trends toward the target: new P's are added
// as demand warrants; excess P's wind down after an idle timeout.
// If never called, reads CSP_MAXPROCS from the environment (default:
// hardware concurrency).
void set_maxprocs(int n);

// Shut down the runtime, joining worker threads and draining pools.
// The next schedule/spawn call will re-initialize the runtime using
// set_maxprocs or CSP_MAXPROCS.
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
template <typename T> class weak_writer;
template <typename T> class weak_reader;

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

template <typename T = poke_t>
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
    template <typename U>
        requires std::is_convertible_v<T, U>
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
    struct alignas(T) aligned_buf { mutable std::byte data[sizeof(T)]; };
    aligned_buf buf_;
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
    static internal::ChanOp chanop() { return {{}, nullptr}; }
    static void transfer(void*, void*) {}
    static void disarm() {}
};
inline constexpr none_t none{};

namespace detail {
template <typename T> struct is_chan_op<chan_op<T>> : std::true_type {};
template <> struct is_chan_op<none_t> : std::true_type {};
}


// Forward declarations for request/response support.
template <typename Req, typename Resp> struct request;
template <typename> struct is_request;

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

    // Blocking call for request<Req, Resp> writers.
    // w(req) sends the request and blocks for the response.
    template <typename U = T>
        requires is_request<U>::value
    typename U::reply_type operator()(typename U::value_type req);

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
    friend class weak_writer<T>;
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
        requires std::is_convertible_v<T, U>
    chan_op<T> operator>>(U & u) const {
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
        [[maybe_unused]] bool ok = static_cast<bool>(*this >> t);
        assert(ok && "single() called on exhausted reader");
        T discard;
        [[maybe_unused]] bool extra = static_cast<bool>(*this >> discard);
        assert(!extra && "single() reader produced more than one value");
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
    friend class weak_reader<T>;
};

// Weak endpoint reference: keeps slot memory alive without contributing to
// death detection. Like std::weak_ptr, use try_lock() to attempt upgrade
// to a strong reference.
template <typename T = poke_t>
class weak_writer {
public:
    weak_writer() = default;
    explicit weak_writer(writer<T> const& w) : w_(w.internal_writer()) {
        if (w_) internal::writer_weak_addref(w_);
    }
    weak_writer(weak_writer const&) = delete;
    weak_writer(weak_writer&& o) : w_(o.w_) { o.w_ = {}; }
    ~weak_writer() { if (w_) internal::writer_weak_release(w_); }

    weak_writer& operator=(weak_writer const&) = delete;
    weak_writer& operator=(weak_writer&& o) {
        if (w_) internal::writer_weak_release(w_);
        w_ = o.w_;
        o.w_ = {};
        return *this;
    }

    // Upgrade to a strong reference. Returns an empty writer if the
    // endpoint is dead (refcount already 0).
    writer<T> try_lock() const {
        if (w_ && internal::try_upgrade_weak_writer(w_)) {
            writer<T> result;
            result.w_ = w_;
            return result;
        }
        return {};
    }

private:
    internal::WriterRef w_;
    friend class writer<T>;
};

template <typename T = poke_t>
class weak_reader {
public:
    weak_reader() = default;
    explicit weak_reader(reader<T> const& r) : r_(r.internal_reader()) {
        if (r_) internal::reader_weak_addref(r_);
    }
    weak_reader(weak_reader const&) = delete;
    weak_reader(weak_reader&& o) : r_(o.r_) { o.r_ = {}; }
    ~weak_reader() { if (r_) internal::reader_weak_release(r_); }

    weak_reader& operator=(weak_reader const&) = delete;
    weak_reader& operator=(weak_reader&& o) {
        if (r_) internal::reader_weak_release(r_);
        r_ = o.r_;
        o.r_ = {};
        return *this;
    }

    // Upgrade to a strong reference. Returns an empty reader if the
    // endpoint is dead (refcount already 0).
    reader<T> try_lock() const {
        if (r_ && internal::try_upgrade_weak_reader(r_)) {
            reader<T> result;
            result.r_ = r_;
            return result;
        }
        return {};
    }

private:
    internal::ReaderRef r_;
    friend class reader<T>;
};

// Vulture-only endpoint wrapper: allows death-watching and bool check,
// but not reading or writing.  Used for lifecycle observation endpoints
// like done() and spawn() handles where data access is meaningless.
//
//   auto handle = closer(spawn(f));  // convert reader to closer
//   prialt(~handle, ...);            // death-watch only
//   if (handle) { ... }              // still alive?
//
// Dropping a closer signals the other side (same as dropping the
// underlying endpoint).
template <typename EP>
class closer {
public:
    closer() = default;
    explicit closer(EP ep) : ep_(std::move(ep)) {}
    closer(closer&&) = default;
    closer& operator=(closer&&) = default;
    closer(closer const&) = delete;
    closer& operator=(closer const&) = delete;

    explicit operator bool() const { return bool(ep_); }
    auto operator~() const { return ~ep_; }

    // Reset (drop the endpoint).
    closer& operator=(std::nullptr_t) { ep_ = {}; return *this; }

    // Access the underlying endpoint.  Use sparingly — the point of
    // closer is to restrict the interface.
    EP& endpoint() & { return ep_; }
    EP const& endpoint() const & { return ep_; }
    EP&& endpoint() && { return std::move(ep_); }

private:
    EP ep_;
};

// Deduction guide: closer(reader<T>) -> closer<reader<T>>.
template <typename EP>
closer(EP) -> closer<EP>;

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

    explicit chan(size_t capacity);

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
// Waiters blocked in prialt when a swap occurs are handled correctly:
// swap_slots wakes all waiters on both channels with a special signal
// (INT_MIN), causing prialt to re-resolve each chanop's channel pointer
// through its slot and re-scan.  This means fuse/split can be applied
// mid-flight without stale waiters or false death signals.
//
// The two sequential swaps create a brief intermediate state under M:N
// concurrency, but this is benign.  Between the first and second swap, the
// topology is inconsistent (one pair redirected, the other not yet) but
// coherent — every slot points to a valid channel with valid waiter lists.
// A concurrent operation hitting the half-swapped topology either targets the
// already-redirected endpoint (correct) or the not-yet-redirected one (old
// channel, blocks until the second swap completes and the topology settles).
// The worst case is a transient stall, never data loss or corruption.
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

// Pipe operator: fuse w's output into r's input.
// Equivalent to fuse(w, r) — "data flows from left to right."
template <typename T>
void operator|(writer<T>& w, reader<T>& r) {
    fuse(w, r);
}

// --- Request/response (RPC over channels) ---

// A request bundled with a reply channel.  The caller creates a one-shot
// chan<Resp>, sends the writer half with the request, and reads from its
// private reader.  This allows concurrent callers on the same request
// channel without stealing each other's responses.
template <typename Req, typename Resp>
struct request {
    using value_type = Req;
    using reply_type = Resp;
    Req value;
    writer<Resp> reply;
};

// Trait to detect request<Req, Resp>.
template <typename> struct is_request : std::false_type {};
template <typename Req, typename Resp>
struct is_request<request<Req, Resp>> : std::true_type {};

// Send a request and return a reader for the response (non-blocking).
//   call(w, key).read()              — block for response
//   prialt(call(w, key) >> val, ...) — multiplex with other work
//   auto r1 = call(w, k1);           — fire-and-collect-later
//   auto r2 = call(w, k2);
//   auto v1 = r1.read(); auto v2 = r2.read();
template <typename Req, typename Resp>
reader<Resp> call(writer<request<Req, Resp>>& w, Req req) {
    chan<Resp> reply;
    w << request<Req, Resp>{std::move(req), std::move(reply.w)};
    return std::move(reply.r);
}

// Out-of-line definition of writer::operator() for request types.
template <typename T>
template <typename U>
    requires is_request<U>::value
typename U::reply_type writer<T>::operator()(typename U::value_type req) {
    return call(*this, std::move(req)).read();
}

// Tap: splice a pass-through observer into the channel between w and r.
// Returns a reader whose values mirror every value flowing from w to r.
// Both the tap reader and the original reader must be consumed for the
// pipeline to flow (the forwarder writes to the tap first, then forwards).
//
// When the tap reader dies (~tw fires), the forwarder fuses w and r back
// together, restoring the direct path. Weak refs to w/r keep the slots
// alive without masking natural pipeline death.
template <typename T>
reader<T> tap(writer<T>& w, reader<T>& r) {
    chan<T> a, b, tap_ch;

    // Take weak refs to original w/r slots BEFORE the swap redirects them.
    weak_writer<T> ww(w);
    weak_reader<T> wr(r);

    // Split: w → [B], [A] → r.  Original channel dies.
    swap(w, std::move(a.r), std::move(b.w), r);

    // Forwarder: reads from B (what w writes to), writes to A (what r reads
    // from) and to tap_ch.  Holds weak refs to the original w/r for fuse-back.
    // When ~tw fires (tap reader died), upgrades weak refs and fuses w/r.
    // Since the weak refs don't contribute to death detection, ~aw and br
    // can properly detect natural pipeline death.
    spawn([br = std::move(b.r), aw = std::move(a.w),
           tw = std::move(tap_ch.w),
           ww = std::move(ww), wr = std::move(wr)]() mutable {
        for (T t;;) {
            int i = prialt(~tw, br >> t);
            if (i == ~0) {
                // Tap reader died. Upgrade weak refs and fuse back.
                auto wc = ww.try_lock();
                auto rc = wr.try_lock();
                if (wc && rc) fuse(wc, rc);
                return;
            }
            if (i < 0) return;  // upstream dead

            if (!(tw << t)) {
                // Tap reader died during write.
                auto wc = ww.try_lock();
                auto rc = wr.try_lock();
                if (wc && rc) {
                    fuse(wc, rc);
                    wc << t;  // deliver in-flight value
                }
                return;
            }

            int j = prialt(~tw, aw << t);
            if (j == ~0) {
                // Tap reader died while forwarding.
                auto wc = ww.try_lock();
                auto rc = wr.try_lock();
                if (wc && rc) {
                    fuse(wc, rc);
                    wc << t;  // deliver in-flight value
                }
                return;
            }
            if (j < 0) return;  // downstream dead
        }
    });

    return std::move(tap_ch.r);
}

// Splice: insert a user-defined filter between w and r.
// The filter function f(reader<T>, writer<T>) receives internal endpoints
// created by splitting the original channel and runs its own forwarding
// loop. When f returns -- for any reason (normal exit, upstream death,
// downstream death, exception, or filter decision) -- w and r are
// automatically fused back together, restoring the direct path.
//
// Weak refs prevent the forwarder from masking natural pipeline death
// (same pattern as tap).
template <typename T, typename F>
void splice(writer<T>& w, reader<T>& r, F&& f) {
    chan<T> a, b;

    // Take weak refs BEFORE the swap redirects slots.
    weak_writer<T> ww(w);
    weak_reader<T> wr(r);

    // Split: w → [B], [A] → r.
    swap(w, std::move(a.r), std::move(b.w), r);

    // Forwarder imp: runs f with copies of the internal endpoints,
    // keeping the originals alive so the intermediate channels survive
    // until after fuse-back.  Without this, the filter's endpoint
    // destruction would kill the intermediate channels before the fuse
    // can redirect w and r, causing the producer to see a dead channel.
    spawn([br = std::move(b.r), aw = std::move(a.w),
           f = std::forward<F>(f),
           ww = std::move(ww), wr = std::move(wr)]() mutable {
        try {
            f(br.copy(), aw.copy());
        } catch (...) {
            auto wc = ww.try_lock();
            auto rc = wr.try_lock();
            if (wc && rc) fuse(wc, rc);
            throw;
        }
        auto wc = ww.try_lock();
        auto rc = wr.try_lock();
        if (wc && rc) fuse(wc, rc);
        // br and aw destroyed here — intermediate channels die after fuse.
    });
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
    using SD = spawn_data<F>;
    std::unique_ptr<SD> sd{static_cast<SD *>(data)};
    std::exception_ptr ex;
    try {
        auto f = std::move(sd->f);
        f();
    } catch (...) {
        ex = std::current_exception();
    }
    // Channel send must be OUTSIDE the catch block.  In M:N mode,
    // prialt_begin can suspend the imp (context-switch).  If it
    // resumes on a different OS thread, __cxa_end_catch would run
    // on that thread while __cxa_begin_catch registered the
    // exception on the original thread's __cxa_eh_globals —
    // corrupting the C++ exception runtime state (SIGSEGV in
    // __cxa_end_catch at address 0x0).
    if (ex) {
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

template <std::ranges::input_range R>
    requires std::is_same_v<std::ranges::range_value_t<R>, reader<std::exception_ptr>>
void join(R&& handles) {
    std::exception_ptr first;
    for (auto& h : handles) {
        std::exception_ptr ep;
        if (h >> ep && !first) first = ep;
    }
    if (first) std::rethrow_exception(first);
}

template <typename... Rs>
    requires (sizeof...(Rs) > 1 && (std::is_same_v<std::decay_t<Rs>, reader<std::exception_ptr>> && ...))
void join(Rs&&... rs) {
    std::exception_ptr first;
    auto try_join = [&](auto& h) {
        std::exception_ptr ep;
        if (h >> ep && !first) first = ep;
    };
    (try_join(rs), ...);
    if (first) std::rethrow_exception(first);
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
    requires (is_chan_op<std::decay_t<Ops>>::value && ...)
inline int typed_alt(Ops &&... ops) {
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
    requires (detail::is_chan_op<std::decay_t<Ops>>::value && ...)
inline int alt(Ops &&... ops) {
    return detail::typed_alt<&internal::alt_begin>(std::forward<Ops>(ops)...);
}

template <typename... Ops>
    requires (detail::is_chan_op<std::decay_t<Ops>>::value && ...)
inline int prialt(Ops &&... ops) {
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

// --- chan | composition ---

// reader | chan → reader (forward reader into buffer)
template <typename T>
reader<T> operator|(reader<T> r, chan<T> ch) {
    spawn([in = std::move(r), out = std::move(ch.w)] {
        T v;
        while (in >> v) {
            if (!(out << std::move(v))) return;
        }
    });
    return std::move(ch.r);
}

// chan | writer → writer (forward buffer into writer)
template <typename T>
writer<T> operator|(chan<T> ch, writer<T> w) {
    spawn([in = std::move(ch.r), out = std::move(w)] {
        T v;
        while (in >> v) {
            if (!(out << std::move(v))) return;
        }
    });
    return std::move(ch.w);
}

// --- buffered channel constructor ---

template <typename T>
chan<T>::chan(size_t capacity) {
    if (capacity == 0)
        throw std::invalid_argument("buffer capacity must be at least 1");
    auto ch = spawn_filter<T>([capacity](reader<T> in, writer<T> out) {
        internal::descr("buffer");
        detail::RingBuffer<T> buf(capacity);
        for (;;) {
            switch (alt(buf.full()  ? ~in  : in  >> buf.next(),
                        buf.empty() ? ~out : out << buf.front())) {
            case 0:  buf.push(); break;
            case ~0:
                while (!buf.empty() && out << std::move(buf.front())) buf.pop();
                return;
            case 1:  buf.pop(); break;
            case ~1: return;
#ifdef _MSC_VER
            default: __assume(0);
#else
            default: __builtin_unreachable();
#endif
            }
        }
    });
    w = std::move(ch.w);
    r = std::move(ch.r);
}

}

