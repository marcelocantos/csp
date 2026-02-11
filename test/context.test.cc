#include <doctest/doctest.h>

#include <csp/fcontext.h>

#include "simple_stack_allocator.hpp"

#include <functional>
#include <memory>

typedef boost::context::simple_stack_allocator<8 << 20, 64 << 10, 8 << 10> stack_allocator;

static constexpr auto stksize = stack_allocator::def_stack;

// Shared caller context for trampoline-based helpers.
static csp::fcontext_t g_caller;

// Trampoline: stores caller context in g_caller, invokes std::function from data.
static void trampoline(csp::transfer_t t) {
    g_caller = t.fctx;
    auto f = std::unique_ptr<std::function<void()>>((std::function<void()> *)t.data);
    (*f)();
}

static csp::fcontext_t make_task(stack_allocator & alloc) {
    void * sp = alloc.allocate(stksize);
    return csp::make_fcontext(sp, stksize, trampoline);
}

static csp::transfer_t jmpf(csp::fcontext_t ctx, std::function<void()> && f) {
    return csp::jump_fcontext(ctx, new std::function<void()>(std::move(f)));
}

//----------------------------------------------------------------
// Basic

TEST_CASE("Context - Basic") {
    stack_allocator alloc;
    bool ran = false;

    void * sp = alloc.allocate(stksize);
    auto ctx = csp::make_fcontext(sp, stksize, [](csp::transfer_t t) {
        *(bool *)t.data = true;
        csp::jump_fcontext(t.fctx, nullptr);
    });

    CHECK_FALSE(ran);
    csp::jump_fcontext(ctx, &ran);
    CHECK(ran);
}

//----------------------------------------------------------------
// StdFunction

TEST_CASE("Context - StdFunction") {
    stack_allocator alloc;
    bool ran_f1 = false;

    auto task = make_task(alloc);

    CHECK_FALSE(ran_f1);
    jmpf(task, [&]() {
        ran_f1 = true;
        csp::jump_fcontext(g_caller, nullptr);
    });
    CHECK(ran_f1);
}

//----------------------------------------------------------------
// AutoReturn

TEST_CASE("Context - AutoReturn") {
    stack_allocator alloc;
    bool ran_f1 = false;

    auto task = make_task(alloc);
    auto task_ctx = jmpf(task, [&]() {
        auto t = csp::jump_fcontext(g_caller, nullptr);
        ran_f1 = true;
        csp::jump_fcontext(t.fctx, nullptr);
    }).fctx;

    CHECK_FALSE(ran_f1);
    csp::jump_fcontext(task_ctx, nullptr);
    CHECK(ran_f1);
}

//----------------------------------------------------------------
// PingPong

TEST_CASE("Context - PingPong") {
    stack_allocator alloc;
    csp::fcontext_t ping, pong;

    // Spawn helper that passes the full transfer_t to f, so f can
    // update context variables from the received fctx.
    auto spawn = [&](std::function<void *(csp::transfer_t)> f) -> csp::fcontext_t {
        auto task = make_task(alloc);
        return jmpf(task, [f = std::move(f)]() {
            auto t = csp::jump_fcontext(g_caller, nullptr);
            auto result = f(t);
            csp::jump_fcontext(t.fctx, result);
        }).fctx;
    };

    ping = spawn([&](csp::transfer_t initial) -> void * {
        intptr_t i = (intptr_t)initial.data;
        while (i) {
            auto t = csp::jump_fcontext(pong, (void *)i);
            pong = t.fctx;
            i = (intptr_t)t.data;
        }
        return (void *)i;
    });

    pong = spawn([&](csp::transfer_t initial) -> void * {
        ping = initial.fctx;  // update with ping's current context
        intptr_t i = (intptr_t)initial.data;
        while (i) {
            auto t = csp::jump_fcontext(ping, (void *)(i - 1));
            ping = t.fctx;
            i = (intptr_t)t.data;
        }
        return (void *)i;
    });

    auto result = csp::jump_fcontext(ping, (void *)10);
    CHECK_EQ(0L, (intptr_t)result.data);
}
