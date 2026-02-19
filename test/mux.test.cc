#include "testutil.h"

#include <string>
#include <variant>
#include <vector>

using namespace csp;
using namespace csp::part;

// --- mux ---

TEST_CASE("Mux - basic two types") {
    RunStats stats;

    using V = std::variant<int, std::string>;

    chan<int> ci;
    chan<std::string> cs;

    auto mr = mux(ci.r.copy(), cs.r.copy()).spawn();

    stats.spawn([w = ci.w.copy()] { w << 42; });
    ci.release();

    stats.spawn([w = cs.w.copy()] { w << std::string("hello"); });
    cs.release();

    // Read two values — order is non-deterministic.
    V v1, v2;
    mr >> v1;
    mr >> v2;

    bool got_int = false, got_str = false;
    for (auto* v : {&v1, &v2}) {
        std::visit([&](auto&& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, int>) {
                CHECK_EQ(42, val);
                got_int = true;
            } else {
                CHECK_EQ("hello", val);
                got_str = true;
            }
        }, *v);
    }
    CHECK(got_int);
    CHECK(got_str);

    mr = {};
    while (csp::internal::run()) { }
}

TEST_CASE("Mux - input death continues") {
    RunStats stats;

    using V = std::variant<int, std::string>;

    chan<int> ci;
    chan<std::string> cs;

    auto mr = mux(ci.r.copy(), cs.r.copy()).spawn();

    // Send one int, then kill int channel.
    stats.spawn([w = ci.w.copy()] { w << 10; });
    ci.release();
    while (csp::internal::run()) { }

    V v;
    mr >> v;
    CHECK(std::holds_alternative<int>(v));
    CHECK_EQ(10, std::get<int>(v));

    // Int channel is dead. Send a string — should still work.
    stats.spawn([w = cs.w.copy()] { w << std::string("world"); });
    cs.release();
    while (csp::internal::run()) { }

    mr >> v;
    CHECK(std::holds_alternative<std::string>(v));
    CHECK_EQ("world", std::get<std::string>(v));

    mr = {};
    while (csp::internal::run()) { }
}

TEST_CASE("Mux - all inputs dead closes output") {
    RunStats stats;

    using V = std::variant<int, double>;

    chan<int> ci;
    chan<double> cd;
    auto r_i = ci.r.copy();
    auto r_d = cd.r.copy();
    ci.release();
    cd.release();

    auto mr = mux(std::move(r_i), std::move(r_d)).spawn();

    // Both inputs are already dead. Output should close.
    V v;
    CHECK_FALSE(bool(mr >> v));

    while (csp::internal::run()) { }
}

TEST_CASE("Mux - output death stops mux") {
    RunStats stats;

    chan<int> ci;
    chan<std::string> cs;

    {
        auto mr = mux(ci.r.copy(), cs.r.copy()).spawn();
        // Drop mr immediately.
    }

    // Writers should fail (mux stopped, readers released).
    // Just ensure clean shutdown.
    ci.release();
    cs.release();
    while (csp::internal::run()) { }
}

// --- demux ---

TEST_CASE("Demux - basic two types") {
    using V = std::variant<int, std::string>;

    auto [w, r] = chan<V>{};

    auto [ri, rs] = demux(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << V{42};
        w << V{std::string("hello")};
        w << V{7};
    });

    int n;
    ri >> n;
    CHECK_EQ(42, n);

    std::string s;
    rs >> s;
    CHECK_EQ("hello", s);

    ri >> n;
    CHECK_EQ(7, n);
}

TEST_CASE("Demux - input closes all outputs") {
    using V = std::variant<int, double>;

    auto [w, r] = chan<V>{};
    auto [ri, rd] = demux(std::move(r));

    // Close input immediately.
    w = {};

    int n;
    CHECK_FALSE(bool(ri >> n));
    double d;
    CHECK_FALSE(bool(rd >> d));
}

TEST_CASE("Demux - output reader drop stops demux") {
    using V = std::variant<int, std::string>;

    auto [w, r] = chan<V>{};
    auto [ri, rs] = demux(std::move(r));

    // Drop one output reader.
    rs = {};

    // Write a string — demux should stop because string output is dead.
    // The int reader should also close.
    csp::spawn([w = std::move(w)] {
        w << V{std::string("boom")};
    });

    int n;
    CHECK_FALSE(bool(ri >> n));
}

// --- roundtrip ---

TEST_CASE("Mux/Demux - roundtrip") {
    RunStats stats;

    chan<int> ci;
    chan<std::string> cs;

    auto mr = mux(ci.r.copy(), cs.r.copy()).spawn();
    auto [ri, rs] = demux(std::move(mr));

    stats.spawn([w = ci.w.copy()] {
        w << 1;
        w << 2;
    });
    ci.release();

    stats.spawn([w = cs.w.copy()] {
        w << std::string("a");
        w << std::string("b");
    });
    cs.release();

    // Collect all values.
    std::vector<int> ints;
    std::vector<std::string> strs;

    csp::spawn([&ints, ri = std::move(ri)] {
        for (int n : ri) ints.push_back(n);
    });

    csp::spawn([&strs, rs = std::move(rs)] {
        for (std::string s : rs) strs.push_back(std::move(s));
    });

    while (csp::internal::run()) { }

    CHECK_EQ(2, ints.size());
    CHECK_EQ(1, ints[0]);
    CHECK_EQ(2, ints[1]);
    CHECK_EQ(2, strs.size());
    CHECK_EQ("a", strs[0]);
    CHECK_EQ("b", strs[1]);
}
