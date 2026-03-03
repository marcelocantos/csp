#include "testutil.h"

#include <algorithm>
#include <numeric>
#include <set>

using namespace csp;
using namespace csp::part;

TEST_CASE("Random - UniformInt") {
    RunStats stats;
    auto r = rand::uniform_int(1, 6).spawn();
    for (int i = 0; i < 1000; ++i) {
        int v = r.read();
        CHECK(v >= 1);
        CHECK(v <= 6);
    }
}

TEST_CASE("Random - UniformInt seeded") {
    RunStats stats;
    auto r1 = rand::uniform_int(0, 99, std::mt19937_64{42}).spawn();
    auto r2 = rand::uniform_int(0, 99, std::mt19937_64{42}).spawn();
    for (int i = 0; i < 100; ++i) {
        CHECK(r1.read() == r2.read());
    }
}

TEST_CASE("Random - UniformReal") {
    RunStats stats;
    auto r = rand::uniform_real(0.0, 1.0).spawn();
    for (int i = 0; i < 1000; ++i) {
        double v = r.read();
        CHECK(v >= 0.0);
        CHECK(v < 1.0);
    }
}

TEST_CASE("Random - Bernoulli") {
    RunStats stats;
    auto r = rand::bernoulli(0.5).spawn();
    int trues = 0;
    for (int i = 0; i < 1000; ++i) {
        if (r.read()) ++trues;
    }
    // With 1000 trials at p=0.5, expect ~500. Allow wide margin.
    CHECK(trues > 200);
    CHECK(trues < 800);
}

TEST_CASE("Random - Bernoulli extremes") {
    RunStats stats;
    auto r0 = rand::bernoulli(0.0).spawn();
    for (int i = 0; i < 100; ++i) {
        CHECK_FALSE(r0.read());
    }

    auto r1 = rand::bernoulli(1.0).spawn();
    for (int i = 0; i < 100; ++i) {
        CHECK(r1.read());
    }
}

TEST_CASE("Random - Normal") {
    RunStats stats;
    auto r = rand::normal(100.0, 10.0).spawn();
    double sum = 0;
    int n = 1000;
    for (int i = 0; i < n; ++i) {
        sum += r.read();
    }
    double mean = sum / n;
    // Mean should be near 100. Allow generous margin.
    CHECK(mean > 90.0);
    CHECK(mean < 110.0);
}

TEST_CASE("Random - Choice") {
    RunStats stats;
    auto r = rand::choice<int>({10, 20, 30}).spawn();
    std::set<int> seen;
    for (int i = 0; i < 100; ++i) {
        int v = r.read();
        CHECK((v == 10 || v == 20 || v == 30));
        seen.insert(v);
    }
    // After 100 picks from 3 values, should have seen all three.
    CHECK(3 == seen.size());
}

TEST_CASE("Random - Choice container") {
    RunStats stats;
    std::vector<std::string> words = {"alpha", "beta", "gamma"};
    auto r = rand::choice<std::string>(words).spawn();
    std::set<std::string> seen;
    for (int i = 0; i < 100; ++i) {
        std::string v = r.read();
        CHECK((v == "alpha" || v == "beta" || v == "gamma"));
        seen.insert(v);
    }
    CHECK(3 == seen.size());
}

TEST_CASE("Random - Shuffle") {
    RunStats stats;
    // Shuffle a sequence of 20 elements through a buffer of 5.
    auto r = (count(1, 21) | rand::shuffle<int>(5)).spawn();
    std::vector<int> got;
    for (int v : r) {
        got.push_back(v);
    }
    // Must be a permutation of 1..20.
    CHECK(20 == got.size());
    auto sorted = got;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < 20; ++i) {
        CHECK(i + 1 == sorted[i]);
    }
}

TEST_CASE("Random - Shuffle seeded") {
    RunStats stats;
    auto r1 = (count(1, 11)
        | rand::shuffle<int>(3, std::mt19937_64{99})).spawn();
    auto r2 = (count(1, 11)
        | rand::shuffle<int>(3, std::mt19937_64{99})).spawn();
    for (int i = 0; i < 10; ++i) {
        CHECK(r1.read() == r2.read());
    }
}

TEST_CASE("Random - Shuffle small input") {
    RunStats stats;
    // Input smaller than buffer size.
    auto r = (count(1, 4) | rand::shuffle<int>(10)).spawn();
    std::vector<int> got;
    for (int v : r) {
        got.push_back(v);
    }
    CHECK(3 == got.size());
    auto sorted = got;
    std::sort(sorted.begin(), sorted.end());
    CHECK(1 == sorted[0]);
    CHECK(2 == sorted[1]);
    CHECK(3 == sorted[2]);
}
