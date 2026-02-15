#ifndef INCLUDED__csp__microthread_h
#define INCLUDED__csp__microthread_h

#include <csp/internal/mt_log.h>

#include <cassert>
#include <exception>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif


/*-------------------------------------------------------------------
 * Microthreads
 */

typedef void (*csp_entry_f)(void *);

/* Invoke this from high up in the application stack, preferably main.
 * Evaluates to non-zero iff the initialization process succeeded. */
#define csp_init(stacksize) (csp__internal__init(_alloca((stacksize)), (stacksize)))

/* Call from main so csp can tell where the main stack is located. */
/* Not currently required. */
/* void csp_main(); */

/* Create a new microthread that calls entry(data).
 * Return non-zero iff the thread was created successfully. */
int csp_spawn(csp_entry_f entry, void * data);

/* Run the currently scheduled microthread until it yields, then schedule
 * another microthread, but return without running it.
 * Return non-zero iff there remain threads that are ready to run. */
int csp_run();

/* Yield control so other microthreads can run. Does nothing outside a
 * microthread. */
void csp_yield();

/* Provide a printf'ed status message for use when logging the current
 * microthread. Formatted messages are truncated to <= 31 bytes. */
void csp_descr(char const * fmt, ...);


/*-------------------------------------------------------------------
 * Memory management
 */

/* Asserts that p is not on the stack. Returns p iff assertion succeeds. */
void * csp_off_stack(void * p);


/*-------------------------------------------------------------------
 * Channels
 */

/* The internal structure of the following is fake. It is a poor
 * man's static type checker. */
typedef struct csp_tag_writer { char can_wait; } * csp_writer;
typedef struct csp_tag_reader { char can_wait; } * csp_reader;

/* Create a channel, populating the i/o-let parameters. The copy function
 * is used to copy csp_write's src parameter into a temporary slot.
 * Return non-zero iff success. */
int csp_chan(csp_writer * w, csp_reader * r, void (* tx)(void * src, void * dst));

/* Add and release refcount on writers and readers. */
csp_writer csp_writer_addref (csp_writer w);
void        csp_writer_release(csp_writer w);
csp_reader csp_reader_addref (csp_reader r);
void        csp_reader_release(csp_reader r);

void csp_chdescr(void * ch, char const * descr);

/* The following operations must be called from inside a microthread. */

/* Prepare a ochan or ichan for csp_(pri)alt. */
typedef struct csp_tag_waiter { char can_wait; } * csp_waiter;
enum { csp_dead = 2, csp_ready = csp_dead | 1 };
#define csp_wait(obj) ((csp_waiter)((uintptr_t)&(obj)->can_wait | ((int)::csp_ready << 1) | 8))
#define csp_wait_dead(obj) ((csp_waiter)((uintptr_t)&(obj)->can_wait | ((int)::csp_dead << 1) | 8))

typedef struct csp_tag_chanop {
    csp_waiter waiter;
    void * message;
} csp_chanop;

/* Block until at least one end-point is signalled, then return a
 * signalled end-point.
 *
 * A signalled writer returns after the reader has processed the
 * message, and may thus safely send stack object addresses.
 *
 * If several waitops are ready at the same time, alt chooses randomly
 * and is thus fairer, whereas prialt chooses the lowest index and is
 * more efficient. */
int csp_alt   (csp_chanop const * waitops, int count, int nowait);
int csp_prialt(csp_chanop const * waitops, int count, int nowait);

/* Block the current microthread until the given deadline (nanoseconds since
 * steady_clock epoch). */
void csp_sleep_until(int64_t deadline_ns);

/* Don't call these. */
int csp__internal__init(void* stack, int stacksize);
char const * csp__internal__getchdescr(void* ch);
char const * csp__internal__getchflags(void* ch);


#ifdef __cplusplus
}

#include <array>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/iterator/iterator_facade.hpp>


#define CSP_DESCR_CHAN__(a) do { CSP_LOG(g_descrlog, "%s = %X:%s", #a, uintptr_t(*(a)) >> 4, uintptr_t(*(a)) & 1 ? "R" : "W"); (a).descr(#a); } while (false)
#define CSP_DESCR_CHAN_(a, ...) CSP_DESCR_CHAN__(a); CSP_DESCR_CHAN_(##__VA_ARGS__);
#define CSP_DESCR_CHAN(a, ...) do { CSP_DESCR_CHAN_(a, ##__VA_ARGS__); } while (false)

#define CSP_DESCR_(F) do { CSP_LOG(g_descrlog, "%s", #F); csp_descr(#F); } while (false)
#define BRAC_DESCR(F, ...) do { CSP_DESCR_(F); CSP_DESCR_CHAN(##__VA_ARGS__); } while (false)


namespace csp
{

    namespace detail {

        extern Logger g_descrlog;

    }

    void set_scheduler(std::function<void()> f);
    void schedule();

    // Initialize the M:N runtime with the given number of processors (0 = auto).
    // If never called, auto-initializes with 1 processor (single-threaded).
    void init_runtime(int num_procs = 0);
    void shutdown_runtime();

    class microthread_error : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // TODO: Make this work again.
    template <typename T>
    inline
    bool tracer_test(T const &, T const &) { return false; }

    template <typename T>
    class tracer {
    public:
        static const T& set(const T& t) { return t_ = t; }
        static bool test(const T& t) { return tracer_test(t, t_); }

    private:
        static T t_;
    };

    template <typename T>
    T tracer<T>::t_;

    namespace { Logger tracer_log("tracer"); }

    // Surrogate for empty-message channels.
    // - Boost took none.
    // - cpplinq squatted on empty.
    // - ObjC owns nil.
    // - null would get confused with NULL.
    extern struct poke_t {
        explicit operator bool() const { return false; }
    } poke;

    class action {
    public:
        using cleanup_f = void (*)(void *);

        action() = default;
        action(action && a)
            : chanop_(a.chanop_)
            , cleanup_(a.cleanup_)
            , active_{a.active_ = false}
        {
            a.chanop_ = {nullptr, nullptr};
            a.cleanup_ = nullptr;
        }
        action(action const &) = delete;

        action(csp_chanop c, cleanup_f cleanup)
            : chanop_(c)
            , cleanup_(cleanup)
        {
        }

        ~action() {
            if (active_) {
                csp_prialt(&chanop_, 1, false);
            }
            if (cleanup_) {
                cleanup_(chanop_.message);
            }
        }

        action & operator=(action && a) {
            chanop_ = a.chanop_;
            cleanup_ = a.cleanup_;
            active_ = a.active_ = false;
            a.chanop_ = {nullptr, nullptr};
            a.cleanup_ = nullptr;
            return *this;
        }
        action & operator=(action const &) = delete;

        explicit operator bool() const {
            active_ = false;
            return csp_prialt(&chanop_, 1, false) > 0;
        }

        csp_chanop chanop() const { return chanop_; }

        bool empty() const { return !chanop_.waiter; }

    private:
        csp_chanop chanop_ = {nullptr, nullptr};
        cleanup_f cleanup_;
        mutable bool active_ = true;
    };

    namespace detail {

        template <typename I>
        inline
        void insert_actions(I) { }

        template <typename I, typename... Actions>
        inline
        void insert_actions(I i, action && a, Actions &&... aa) {
            *i = std::move(a);
            insert_actions(++i, std::forward<Actions>(aa)...);
        }

    }

    template <typename... Actions>
    auto action_list(Actions &&... aa) {
        std::vector<action> actions;
        actions.reserve(sizeof...(aa));
        detail::insert_actions(back_inserter(actions), std::forward<Actions>(aa)...);
        return actions;
    }

    template <typename T> class channel;

    template <typename T>
    struct is_tunnelable_via_ptr {
        static const bool value = false; //std::is_pod<T>::value && sizeof(T) <= sizeof(void *);
    };

    namespace detail {

        template <typename T>
        action writer_action(csp_writer w, T const & t, std::enable_if_t<is_tunnelable_via_ptr<T>::value> * = nullptr) {
            union {
                void * p;
                T t;
            } u;
            u.t = t;
            return {csp_chanop{csp_wait(w), u.p}, nullptr};
        }

        template <typename T>
        action writer_action(csp_writer w, T const & t, std::enable_if_t<!is_tunnelable_via_ptr<T>::value> * = nullptr) {
            return {csp_chanop{csp_wait(w), new T(t)}, [](void * p) { delete (T *)(p); }};
        }

        template <typename T>
        void tx_message_(void * src, void * dst, std::enable_if_t<!is_tunnelable_via_ptr<T>::value> * = nullptr) {
            T * p = static_cast<T *>(src);
            *static_cast<T *>(dst) = std::move(*p);
        }

        template <typename T>
        void tx_message_(void * src, void * dst, std::enable_if_t<is_tunnelable_via_ptr<T>::value> * = nullptr) {
            union {
                void * p;
                T t;
            } u;
            u.p = src;
            *static_cast<T *>(dst) = std::move(u.t);
        }

        template <typename T>
        void tx_message(void * src, void * dst) {
            tx_message_<T>(src, dst);
        }

    }

    template <typename T = poke_t>
    class writer {
    public:
        static writer dead();

        writer() = default;
        writer(writer const & w) : w_(w.w_) { if (w_) csp_writer_addref(w_); }
        writer(writer && w) : w_(w.w_) { w.w_ = nullptr; }
        ~writer() {
            if (w_) {
                csp_writer_release(w_);
            }
        }

        writer& operator=(writer const & w) {
            if (&w != this) {
                writer w_(w);
                swap(w_);
            }
            return *this;
        }
        writer<T>& operator=(writer && w) {
            if (w_) csp_writer_release(w_);
            w_ = w.w_;
            w.w_ = nullptr;
            return *this;
        }
        void swap(writer& w) {
            csp_writer tmp = w_;
            w_ = w.w_;
            w.w_ = tmp;
        }

        bool operator==(const writer& w) const { return w_ == w.w_; }
        bool operator!=(const writer& w) const { return !(*this == w); }
        explicit operator bool() const { return bool(w_); }

        void descr(const char* d) const { csp_chdescr(w_, d); }

        action operator<<(T const & t) const { return detail::writer_action(w_, t); }
        action operator<<(T && t) const {
            return {csp_chanop{csp_wait(w_), new T(std::move(t))}, [](void * p) { delete (T *)(p); }};
        }

        action operator~() const {
            return {csp_chanop{csp_wait_dead(w_), nullptr}, nullptr};
        }

        csp_writer internal_writer() const { return w_; }

    private:
        mutable csp_writer w_ = nullptr;

        void assign(csp_writer w) { w_ = w; }

        friend class channel<T>;
    };

    namespace { Logger g_reader_log("reader"); }

    template <typename T = poke_t>
    class reader {
    public:
        static reader dead();

        reader() = default;
        reader(const reader & r) : r_(r.r_) { if (r_) csp_reader_addref(r_); }
        reader(reader && r) : r_(r.r_) { r.r_ = nullptr; }
        ~reader() {
            if (r_) {
                csp_reader_release(r_);
            }
        }

        reader& operator=(reader const & r) {
            if (&r != this) {
                reader r_(r);
                swap(r_);
            }
            return *this;
        }
        reader& operator=(reader && r) {
            if (r_) csp_reader_release(r_);
            r_ = r.r_;
            r.r_ = nullptr;
            return *this;
        }
        void swap(reader& i) {
            csp_reader tmp = r_;
            r_ = i.r_;
            i.r_ = tmp;
        }

        bool operator==(const reader& r) const { return r_ == r.r_; }
        bool operator!=(const reader& r) const { return !(*this == r); }
        explicit operator bool() const  { return bool(r_); }

        void descr(const char* d) { csp_chdescr(r_, d); }

        template <typename U>
        std::enable_if_t<std::is_convertible<T, U>::value, action>
        operator>>(U & u) const {
            return {{csp_wait(r_), &u}, nullptr};
        }
        action operator>>(void * dest) const {
            reader<T> r = *this;
            return {{csp_wait(r_), dest}, nullptr};
        }

        // Connect two channels directly.
        template <typename U>
        auto stream_to(writer<U> out) const {
            return [in = *this, out] {
                for (T t; prialt(~out, in >> t) > 0 && out << t;) { }
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

        class iterator : public boost::iterator_facade<iterator, T const, boost::forward_traversal_tag> {
        public:
            iterator(reader<T> source) : source_(source) {
                if (source_) {
                    increment();
                }
            }

        private:
            reader<T> source_;
            T t_;

            void increment() {
                if (!(source_ >> t_)) {
                    source_ = {};
                }
            }

            bool equal(iterator const & i) const {
                return source_ == i.source_;
            }

            T const & dereference() const { return t_; }

            friend class boost::iterator_core_access;
        };

        iterator begin() const { return {*this}; }
        iterator end() const { return {{}}; }

        action operator~() const {
            return {csp_chanop{csp_wait_dead(r_), nullptr}, nullptr};
        }

        csp_reader internal_reader() const { return r_; }

    private:
        mutable csp_reader r_ = nullptr;

        void assign(csp_reader r) { r_ = r; }

        // Extract the next T into a U reference, returning true iff the channel hasn't died.
        // This function is only visible if T can be converted to U.
        template <typename U>
        std::enable_if_t<std::is_convertible<T, U>::value, bool>
        read_(U & u) const;

        bool read_(void * dest) const;

        friend class channel<T>;
    };

    template <typename T = poke_t>
    class channel {
    public:
        channel() {
            csp_writer w;
            csp_reader r;
            if (csp_chan(&w, &r, &detail::tx_message<T>) == 0) {
                throw microthread_error("channel creation failed");
            }
            w_.assign(w);
            r_.assign(r);
        }
        channel(writer<T> w, reader<T> r) : w_(w), r_(r) { }
        channel(channel const &) = default;
        channel(channel && e) : w_(std::move(e.w_)), r_(std::move(e.r_)) {
            static Logger log("channel/test");
            CSP_LOG(log, "move %p/%p", w_.internal_writer(), r_.internal_reader());
        }

        channel & operator=(channel const &) = default;
        channel & operator=(channel && e) {
            static Logger log("channel/test");
            CSP_LOG(log, "move %p/%p <- %p/%p", w_.internal_writer(), r_.internal_reader(), e.w_.internal_writer(), e.r_.internal_reader());
            w_ = std::move(e.w_);
            CSP_LOG(log, ".... %p/%p <- %p/%p", w_.internal_writer(), r_.internal_reader(), e.w_.internal_writer(), e.r_.internal_reader());
            r_ = std::move(e.r_);
            CSP_LOG(log, "moved %p/%p <- %p/%p", w_.internal_writer(), r_.internal_reader(), e.w_.internal_writer(), e.r_.internal_reader());
            return *this;
        }

        void release() {
            w_ = {};
            r_ = {};
        }

        writer<T> const & operator+() const { return w_; }
        writer<T>       & operator+()       { return w_; }
        reader<T> const & operator-() const { return r_; }
        reader<T>       & operator-()       { return r_; }

        // Move the endpoints.
        writer<T> operator++() { return std::move(w_); }
        reader<T> operator--() { return std::move(r_); }

    private:
        writer<T> w_;
        reader<T> r_;
    };

    template <typename T>
    reader<T> reader<T>::dead() {
        return --channel<T>();
    }

    template <typename T>
    writer<T> writer<T>::dead() {
        return ++channel<T>();
    }


    template <typename T>
    template <typename U>
    std::enable_if_t<std::is_convertible<T, U>::value, bool>
    reader<T>::read_(U& u) const {
        if (!r_) {
            throw microthread_error("Can't read from empty reader");
        }
        if (T * slot = static_cast<T *>(csp_read(r_))) {
            u = std::move(*slot);
            slot->~T();
            return true;
        }
        return false;
    }

    template <typename T>
    bool reader<T>::read_(void * dest) const {
        if (!r_) {
            throw microthread_error("Can't read from empty reader");
        }
        if (T * slot = static_cast<T *>(csp_read(r_))) {
            if (dest) {
                new (dest) T(std::move(*slot));
            }
            slot->~T();
            return true;
        }
        return false;
    }

    template <typename T>
    void make_channel(writer<T> & w, reader<T> & r) {
        channel<T> ch;
        w = ++ch;
        r = --ch;
    }

    // Make a channel for a writer&, returning the matching reader.
    template <typename T>
    reader<T> operator--(writer<T> & w) {
        if (w) {
            throw microthread_error("writer already attached channel");
        }
        channel<T> ch;
        w = ++ch;
        return --ch;
    }

    // Make a channel for a reader&, returning the matching writer.
    template <typename T>
    writer<T> operator++(reader<T> & r) {
        if (r) {
            throw microthread_error("reader already attached to channel");
        }
        channel<T> ch;
        r = --ch;
        return ++ch;
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
        if (!csp_spawn(detail::spawn_entry<F>, sd)) {
            throw microthread_error("spawn failed");
        }
        return r;
    }

    inline void join(reader<std::exception_ptr> r) {
        std::exception_ptr ep;
        if (r >> ep) {
            std::rethrow_exception(ep);
        }
    }

    template <typename T, typename F>
    writer<T> spawn_consumer(F f) {
        writer<T> w;
        spawn([f = std::move(f), r = --w]{
            f(std::move(r));
        });
        return w;
    }

    template <typename T, typename F>
    reader<T> spawn_producer(F && f) {
        reader<T> r;
        spawn([f = std::move(f), w = ++r]{
            f(std::move(w));
        });
        return r;
    }

    template <typename T, typename F>
    channel<T> spawn_filter(F && f) {
        channel<T> in, out;
        spawn([f = std::move(f), in = --in, out = ++out]{
            f(std::move(in), std::move(out));
        });
        return {++in, --out};
    }

    // Range over a producer µthread; propagate exceptions therefrom.
    template <typename T>
    class range {
    public:
        class iterator : public boost::iterator_facade<iterator, T const, boost::forward_traversal_tag> {
        public:
            iterator(range<T> const * source = {}) : source_(source) {
                if (source_ && source_->r_) {
                    increment();
                }
            }

        private:
            range<T> const * source_;
            T t_;

            void increment() {
                if (!(source_->r_ >> t_)) {
                    join(source_->ex_);
                    source_ = {};
                }
                static Logger log("xxx");
            }

            bool equal(iterator const & i) const {
                return source_ == i.source_;
            }

            T const & dereference() const { return t_; }

            friend class boost::iterator_core_access;
        };

        range(reader<T> r, reader<std::exception_ptr> ex) : r_(r), ex_(ex) { }

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
        auto ex = spawn([f, w = ++r]{
            f(std::move(w));
        });
        return {r, ex};
    }

    namespace detail {

        using csp_alt_f = int(csp_chanop const * waiter, int count, int nowait);

        template <detail::csp_alt_f baf>
        inline
        int alt(action const * io, size_t n) {
            std::vector<csp_chanop> chanops;
            chanops.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                chanops.push_back(io[i].chanop());
            }
            return baf(chanops.data(), (int)chanops.size(), 0);
        }

    }

    inline
    int alt(action const * io, size_t n) {
        return detail::alt<csp_alt>(io, n);
    }

    template <int N>
    inline
    int alt(action const (& io)[N]) {
        return alt(io, N);
    }

    inline
    int alt(std::vector<action> const & actions) {
        return alt(&actions.front(), actions.size());
    }

    template <typename... Actions>
    inline
    int alt(action && a, Actions &&... aa) {
        constexpr size_t n = 1 + sizeof...(aa);
        action actions[n] = {std::move(a)};
        detail::insert_actions(actions + 1, std::forward<Actions>(aa)...);
        return alt(actions, n);
    }

    inline
    int prialt(action const * io, size_t n) {
        return detail::alt<csp_prialt>(io, n);
    }

    template <int N>
    inline
    int prialt(action const (& io)[N]) {
        return prialt(io, N);
    }

    inline
    int prialt(std::vector<action> const & actions) {
        return prialt(actions.data(), actions.size());
    }

    template <typename... Actions>
    inline
    int prialt(action && a, Actions &&... aa) {
        constexpr size_t n = 1 + sizeof...(aa);
        action actions[n] = {std::move(a)};
        detail::insert_actions(actions + 1, std::forward<Actions>(aa)...);
        return prialt(actions, n);
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

#endif /* __cplusplus */

#endif // INCLUDED__csp__microthread_h
