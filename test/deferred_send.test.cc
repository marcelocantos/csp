// 🎯T59 — deferred ownership transfer for alt/prialt send arms.
//
// `w << value` captures eagerly: the chan_op takes the value while the
// argument list is built, before prialt_begin picks a winner.  For a
// move-only payload that owns live channel endpoints, a losing send arm
// therefore destroys the payload.  `w << csp::from(value)` borrows instead
// and moves only on the arm that commits.

#include "testutil.h"

#include <utility>

using namespace csp;

namespace {

enum class Signal { pause };

constexpr int payload_id = 7;
constexpr int echo_value = 1234;

// Move-only payload owning a live channel endpoint — the shape that made
// eager capture fatal in the Lifeboat demo (Cargo owning entity endpoints).
struct Payload {
    int id = 0;
    writer<int> sink;

    Payload() = default;
    Payload(int i, writer<int> s) : id(i), sink(std::move(s)) {}
    Payload(Payload const &) = delete;
    Payload & operator=(Payload const &) = delete;
    Payload(Payload &&) = default;
    Payload & operator=(Payload &&) = default;
};

// Compile-time probes.  Written as concepts (not bare requires-expressions)
// so an ill-formed body is a substitution failure rather than a hard error.
template <typename V>
concept borrowable = requires (V && v) { csp::from(std::forward<V>(v)); };

template <typename T, typename V>
concept deferred_sendable = requires (writer<T> & w, V && v) {
    w << csp::from(std::forward<V>(v));
};

}

TEST_SUITE("deferred_send") {

// A control arm wins against a move-only send; the payload survives intact
// and a retry then transfers its ownership exactly once.
TEST_CASE("deferred-send---control-wins-then-retry-delivers-intact-payload") {
    int first_choice = -1, second_choice = -1;
    int delivered_id = -1, echoed = -1;
    bool intact_after_control = false, owned_after_send = true;

    csp::run([&] {
        chan<int> echo;
        chan<Signal> control;
        chan<Payload> out;
        chan<int> ack;

        Payload payload{payload_id, std::move(echo.w)};

        // Nothing reads `out` yet, so the send arm cannot match; the control
        // arm decides the alt as soon as the signal arrives.  `control.w` is
        // retained here so the arm stays live (not dead) for the retry.
        spawn([w = control.w.copy()] { w << Signal::pause; });

        Signal signal{};
        first_choice = prialt(control.r >> signal, out.w << csp::from(payload));
        intact_after_control = bool(payload.sink);

        // Retry with a consumer present: now the send arm is the one that can
        // match, and it must carry the *same* payload, endpoint still live.
        spawn([r = std::move(out.r), a = std::move(ack.w)]() mutable {
            Payload got;
            r >> got;
            got.sink << echo_value;   // proves the endpoint survived the alt
            a << got.id;
        });

        second_choice = prialt(control.r >> signal, out.w << csp::from(payload));
        owned_after_send = bool(payload.sink);

        echoed = echo.r.read();
        delivered_id = ack.r.read();
    });

    CHECK(0 == first_choice);              // control won
    CHECK(intact_after_control);           // payload untouched by the losing arm
    CHECK(1 == second_choice);             // retry sent
    CHECK_FALSE(owned_after_send);         // ownership transferred exactly once
    CHECK(payload_id == delivered_id);
    CHECK(echo_value == echoed);
}

// Negative control: the eager form is still legal C++ (it is the documented
// semantics of `w << std::move(v)`), and it still loses the payload when the
// control arm wins.  This test pins that hazard so the deferred form above
// cannot silently regress into it.
TEST_CASE("deferred-send---negative-control-eager-move-loses-the-payload") {
    int choice = -1;
    bool intact_after_control = true;

    csp::run([&] {
        chan<int> echo;
        chan<Signal> control;
        chan<Payload> out;

        Payload payload{payload_id, std::move(echo.w)};

        spawn([w = control.w.copy()] { w << Signal::pause; });

        Signal signal{};
        choice = prialt(control.r >> signal, out.w << std::move(payload));
        intact_after_control = bool(payload.sink);
    });

    CHECK(0 == choice);
    CHECK_FALSE(intact_after_control);     // the hazard csp::from exists to avoid
}

// Compile-time guards on the borrow.  `csp::from` must take a named lvalue:
// a temporary would die at the end of the full expression, before alt/prialt
// could commit to the borrow, and a const lvalue cannot be moved from.
TEST_CASE("deferred-send---borrow-misuse-is-rejected-at-compile-time") {
    static_assert(borrowable<Payload &>,
                  "csp::from must accept a named lvalue");
    static_assert(!borrowable<Payload>,
                  "csp::from must reject temporaries: the borrow would dangle");
    static_assert(!borrowable<Payload const &>,
                  "csp::from must reject const: the selected arm moves out");
    static_assert(deferred_sendable<Payload, Payload &>,
                  "a borrowed lvalue must compile into a send");
    static_assert(!deferred_sendable<Payload, Payload>,
                  "a borrowed temporary must not compile into a send");
    static_assert(!deferred_sendable<Payload, Payload const &>,
                  "a const borrow must not compile into a send");
    CHECK(true);
}

}
