#include <doctest/doctest.h>

#include <csp/ringbuffer.h>

#include <set>

using namespace csp::detail;

namespace {

    // A type that tracks which instances are alive, catching use of
    // uninitialized or destroyed memory in operator== and operator=.
    struct Tracked {
        static std::set<Tracked const *> & alive() {
            static std::set<Tracked const *> s;
            return s;
        }

        int value;

        explicit Tracked(int v) : value(v) { alive().insert(this); }
        Tracked(Tracked const & o) : value(o.value) { alive().insert(this); }
        Tracked(Tracked && o) noexcept : value(o.value) { alive().insert(this); }

        Tracked & operator=(Tracked const & o) {
            REQUIRE(alive().count(this));
            value = o.value;
            return *this;
        }

        Tracked & operator=(Tracked && o) noexcept {
            REQUIRE(alive().count(this));
            value = o.value;
            return *this;
        }

        ~Tracked() { alive().erase(this); }

        bool operator==(Tracked const & o) const {
            REQUIRE(alive().count(this));
            REQUIRE(alive().count(&o));
            return value == o.value;
        }
    };

}

// ---------------------------------------------------------------------------
// Basic operations
// ---------------------------------------------------------------------------

TEST_CASE("RingBuffer - PushPop") {
    RingBuffer<int> buf;
    buf.push(10);
    buf.push(20);
    buf.push(30);
    CHECK_EQ(3, buf.count());
    CHECK_EQ(10, buf.front()); buf.pop();
    CHECK_EQ(20, buf.front()); buf.pop();
    CHECK_EQ(30, buf.front()); buf.pop();
    CHECK(buf.empty());
}

TEST_CASE("RingBuffer - Emplace") {
    RingBuffer<Tracked> buf;
    buf.emplace(42);
    CHECK_EQ(1, buf.count());
    CHECK_EQ(42, buf.front().value);
    buf.pop();
    CHECK(Tracked::alive().empty());
}

TEST_CASE("RingBuffer - BoundedCapacity") {
    RingBuffer<int> buf(3);
    buf.push(1);
    buf.push(2);
    buf.push(3);
    CHECK(buf.full());
    CHECK_EQ(3, buf.count());
    buf.pop();
    CHECK_FALSE(buf.full());
    buf.push(4);
    CHECK(buf.full());
    // FIFO order preserved through wrap-around.
    CHECK_EQ(2, buf.front()); buf.pop();
    CHECK_EQ(3, buf.front()); buf.pop();
    CHECK_EQ(4, buf.front()); buf.pop();
    CHECK(buf.empty());
}

TEST_CASE("RingBuffer - Iterator") {
    RingBuffer<int> buf;
    buf.push(10);
    buf.push(20);
    buf.push(30);

    int sum = 0;
    for (auto const & v : buf) sum += v;
    CHECK_EQ(60, sum);
}

// ---------------------------------------------------------------------------
// Grow — catches UB from std::move into uninitialized memory
// ---------------------------------------------------------------------------

TEST_CASE("RingBuffer - GrowTracked") {
    // Default unbounded buffer starts at internal size 4.
    // Pushing 5+ items forces a grow.  The old code used std::move
    // (the algorithm) into uninitialized memory, calling move-assignment
    // on an unconstructed Tracked — which Tracked::operator= catches.
    RingBuffer<Tracked> buf;
    for (int i = 0; i < 10; ++i)
        buf.push(Tracked{i});

    // Verify FIFO order survived the grow.
    for (int i = 0; i < 10; ++i) {
        CHECK_EQ(i, buf.front().value);
        buf.pop();
    }
    CHECK(Tracked::alive().empty());
}

TEST_CASE("RingBuffer - GrowWrapped") {
    // Push and pop to move front_ forward, then fill past capacity
    // to exercise grow when the data wraps around.
    RingBuffer<Tracked> buf;
    for (int i = 0; i < 3; ++i) buf.push(Tracked{i});
    for (int i = 0; i < 3; ++i) buf.pop();
    // front_ is now at index 3.  Push 5 items to force grow with wrap.
    for (int i = 0; i < 5; ++i) buf.push(Tracked{100 + i});

    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(100 + i, buf.front().value);
        buf.pop();
    }
    CHECK(Tracked::alive().empty());
}

// ---------------------------------------------------------------------------
// Remove — catches scanning past valid entries
// ---------------------------------------------------------------------------

TEST_CASE("RingBuffer - RemoveNotFound") {
    // Old code scanned size_ (4) slots instead of count_ (2),
    // comparing against uninitialized Tracked entries.
    RingBuffer<Tracked> buf;
    buf.push(Tracked{1});
    buf.push(Tracked{2});
    CHECK_FALSE(buf.remove(Tracked{999}));
    CHECK_EQ(2, buf.count());
}

TEST_CASE("RingBuffer - RemoveFront") {
    RingBuffer<Tracked> buf;
    buf.push(Tracked{1});
    buf.push(Tracked{2});
    buf.push(Tracked{3});
    CHECK(buf.remove(Tracked{1}));
    CHECK_EQ(2, buf.count());
    CHECK_EQ(2, buf.front().value);
}

TEST_CASE("RingBuffer - RemoveBack") {
    RingBuffer<Tracked> buf;
    buf.push(Tracked{1});
    buf.push(Tracked{2});
    buf.push(Tracked{3});
    CHECK(buf.remove(Tracked{3}));
    CHECK_EQ(2, buf.count());
    // FIFO: 1, 2 remain.
    CHECK_EQ(1, buf.front().value); buf.pop();
    CHECK_EQ(2, buf.front().value); buf.pop();
    CHECK(Tracked::alive().empty());
}

TEST_CASE("RingBuffer - RemoveMiddle") {
    RingBuffer<Tracked> buf;
    buf.push(Tracked{1});
    buf.push(Tracked{2});
    buf.push(Tracked{3});
    CHECK(buf.remove(Tracked{2}));
    CHECK_EQ(2, buf.count());
    // Front is still 1.
    CHECK_EQ(1, buf.front().value);
}

// ---------------------------------------------------------------------------
// Destructor — verifies all elements are destroyed
// ---------------------------------------------------------------------------

TEST_CASE("RingBuffer - DestructorCleansUp") {
    {
        RingBuffer<Tracked> buf;
        for (int i = 0; i < 10; ++i) buf.push(Tracked{i});
    }
    CHECK(Tracked::alive().empty());
}

TEST_CASE("RingBuffer - ClearThenDestroy") {
    RingBuffer<Tracked> buf;
    for (int i = 0; i < 5; ++i) buf.push(Tracked{i});
    buf.clear();
    CHECK(Tracked::alive().empty());
    CHECK(buf.empty());
}

// ---------------------------------------------------------------------------
// Wrap-around stress
// ---------------------------------------------------------------------------

TEST_CASE("RingBuffer - WrapAroundStress") {
    // Repeatedly fill and drain a bounded buffer so that front_ and
    // back_ wrap around the internal array many times.
    RingBuffer<int> buf(4);
    for (int round = 0; round < 250; ++round) {
        for (int i = 0; i < 4; ++i) buf.push(round * 4 + i);
        CHECK(buf.full());
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(round * 4 + i, buf.front());
            buf.pop();
        }
        CHECK(buf.empty());
    }
}
