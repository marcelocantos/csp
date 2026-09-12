// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
// Motion imps own the choreography. Every cargo handoff below is a real CSP
// channel operation; the browser interpolates their streamed visual twins.
#include "csp.h"
#include "scene.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lifeboat {
using namespace csp;
using namespace std::chrono_literals;
constexpr int approach_capacity = 8;
constexpr int warehouse_capacity = 10;
constexpr int tram_capacity = 4;
constexpr int actor_count = 6;
constexpr int cargo_limit = approach_capacity + warehouse_capacity + tram_capacity + actor_count;
constexpr int event_capacity = 128;
constexpr int history_limit = 12;
constexpr std::array<const char*, actor_count> names = {
    "Arrival control", "Crane Aster", "Crane Boreal", "Warehouse", "Fabricator", "Tram Meridian"};
enum class Command { snapshot, bay, factory, fault, surge, evacuate, reset, quit };
enum class Signal { toggle, fault, surge, evacuate };
enum class Kind { created, handoff, status, delivered, finished, restarted, violation,
                  entity_started, motion, motion_done, entity_stopped };
struct Event {
    Kind kind;
    int actor = 0, cargo = 0;
    std::string stage;
    Motion motion;
};
enum class MotionAction { move, freeze, stop };
struct MotionOrder {
    MotionAction action = MotionAction::move;
    std::string phase, kind;
    int cargo = 0, actor = 0;
    double seconds = 0;
    time_point when{};
    std::vector<Point> targets;
    writer<bool> done;
};
struct Entity {
    std::string key;
    writer<MotionOrder> commands;
    Entity copy() const { return {key, commands.copy()}; }
};
struct Cargo {
    int id = 0, berth = 0, rack = 0, platform = 0;
    Entity box, ship;
};
struct Item { int id; int actor; std::string stage, from; double at, duration; };
struct ActorState { std::string status = "starting"; int cargo = 0; };
struct Note { double at; int actor; std::string text; };
struct Snapshot {
    int run = 0, created = 0, delivered = 0, active = actor_count;
    int transfers = 0, restarts = 0, violations = 0;
    int animation_active = 0, motion_completed = 0, motion_violations = 0;
    unsigned long motion_serial = 0;
    bool bay_closed = false, factory_paused = false, surge = false;
    double now = 0;
    std::string mode = "running";
    std::array<ActorState, actor_count> actors;
    std::map<int, Item> items;
    std::map<std::string, Motion> motions;
    std::deque<Note> notes;
};
struct Reply { Snapshot state; bool ok = true; };
struct Query { Command command; writer<Reply> reply; };
struct CraneFault {};

// Every visual twin has a conventional typed CSP inbox and its own imp. Its
// imp owns position and time; neither the observer nor browser advances it.
Entity make_entity(std::string key, std::string kind, Point initial, int actor,
                   int cargo, writer<Event> events, time_point epoch, double speed) {
    chan<MotionOrder> commands;
    chan<bool> registered(1);
    spawn([r = std::move(commands.r), events = std::move(events), ready = std::move(registered.w),
           key, kind, initial, actor, cargo, epoch, speed]() mutable {
        auto seconds = [&] { return std::chrono::duration<double>(csp::now() - epoch).count() * speed; };
        auto scaled = [&](double value) { return std::chrono::duration_cast<duration>(std::chrono::duration<double>(value / speed)); };
        Motion current{key, kind, "ready", cargo, actor, 0, seconds(), 0, {{0, initial}, {1, initial}}};
        events << Event{Kind::entity_started, actor, cargo, {}, current};
        ready << true;
        std::optional<Motion> interrupted;
        double interrupted_at = 0;
        std::string completed_phase;
        int completed_cargo = -1;
        bool stop = false;
        while (!stop) {
            MotionOrder order;
            if (!(r >> order)) break;
            if (order.action == MotionAction::stop) { order.done << true; break; }
            if (order.action == MotionAction::freeze) { order.done << true; continue; }
            // Retrying a choreography step never replays a completed movement.
            if (order.phase == completed_phase && order.cargo == completed_cargo) { order.done << true; continue; }
            Point position = interpolate(current, seconds());
            double length = order.seconds;
            auto targets = std::move(order.targets);
            if (interrupted && interrupted->phase == order.phase && interrupted->cargo == order.cargo) {
                targets.clear();
                for (auto const& frame : interrupted->frames) if (frame.t > interrupted_at) targets.push_back(frame.p);
                length = interrupted->duration * (1 - interrupted_at);
            }
            interrupted.reset();
            auto begin = std::max(order.when, csp::now());
            current = {key, order.kind, order.phase, order.cargo, order.actor, 0,
                std::chrono::duration<double>(begin - epoch).count() * speed, length, {{0, position}}};
            if (targets.empty()) targets.push_back(position);
            for (size_t i = 0; i < targets.size(); ++i) current.frames.push_back({double(i + 1) / targets.size(), targets[i]});
            events << Event{Kind::motion, current.actor, current.cargo, {}, current};
            auto end = begin + scaled(length);
            bool complete = false;
            for (;;) {
                MotionOrder next;
                int selected = prialt(r >> next, after(std::max(duration::zero(), end - csp::now())) >> nullptr);
                if (selected == 1) { complete = true; break; }
                if (selected != 0) { stop = true; break; }
                if (next.action == MotionAction::move) { next.done << false; continue; }
                // A fault suspends the actual pose, and remembers only the
                // unfinished waypoints for a supervisor-driven retry.
                interrupted = current;
                const double frozen_at = seconds();
                interrupted_at = current.duration > 0 ? std::clamp((frozen_at - current.at) / current.duration, 0.0, 1.0) : 1;
                Point frozen = interpolate(current, frozen_at);
                current.at = frozen_at; current.duration = 0;
                current.frames = {{0, frozen}, {1, frozen}};
                events << Event{Kind::motion, current.actor, current.cargo, {}, current};
                next.done << true;
                stop = next.action == MotionAction::stop;
                break;
            }
            order.done << complete;
            if (complete) {
                completed_phase = order.phase; completed_cargo = order.cargo;
                events << Event{Kind::motion_done, current.actor, current.cargo, {}, {}};
            }
        }
        current.at = seconds();
        events << Event{Kind::entity_stopped, current.actor, current.cargo, key, current};
    });
    bool ready = false;
    registered.r >> ready;
    return {std::move(key), std::move(commands.w)};
}
struct Move { Entity* entity; std::string kind; std::vector<Point> targets; };

struct Actor {
    int id;
    double speed;
    time_point epoch;
    reader<Signal> control;
    writer<Event> events;
    bool paused = false, stopping = false, surge = false;
    duration scaled(double seconds) const {
        return std::chrono::duration_cast<duration>(std::chrono::duration<double>(seconds / speed));
    }
    void report(Kind kind, int cargo = 0, std::string stage = {}) { events << Event{kind, id, cargo, std::move(stage), {}}; }
    void state(const char* value, int cargo = 0) { report(Kind::status, cargo, value); }
    Entity entity(std::string key, std::string kind, Point p, int cargo = 0) {
        return make_entity(std::move(key), std::move(kind), p, id, cargo, events.copy(), epoch, speed);
    }
    void handle(Signal signal) {
        switch (signal) {
        case Signal::toggle: paused = !paused; break;
        case Signal::surge: surge = !surge; break;
        case Signal::evacuate: stopping = true; paused = false; break;
        case Signal::fault: throw CraneFault{};
        }
    }
    void ready() {
        while (paused) {
            state(id == 4 ? "paused" : "bay closed");
            Signal signal;
            if (!(control >> signal)) { paused = false; stopping = true; return; }
            handle(signal);
        }
    }
    void delay(double seconds) {
        auto end = csp::now() + scaled(seconds);
        while (csp::now() < end) {
            Signal signal;
            if (prialt(control >> signal, after(end - csp::now()) >> nullptr) == 0) handle(signal);
            else break;
        }
    }
    template<class T> bool receive(reader<T>& input, T& value, bool intake = true) {
        for (;;) {
            if (intake) ready();
            state("awaiting cargo");
            Signal signal;
            int choice = prialt(control >> signal, input >> value);
            if (choice == 0) handle(signal);
            else return choice == 1;
        }
    }
    bool send(writer<Cargo>& output, Cargo& cargo) {
        state("backpressure", cargo.id);
        for (;;) {
            Signal signal;
            // Retain the imp endpoints if a control signal wins this alt.
            auto transfer = chan_op<Cargo>(output.internal_writer(), cargo,
                                           chan_op<Cargo>::ref_tag{});
            int choice = prialt(control >> signal, std::move(transfer));
            if (choice == 0) handle(signal);
            else return choice == 1;
        }
    }
    void play(std::string phase, double seconds, int cargo, std::initializer_list<Move> moves) {
        std::vector<reader<bool>> completions;
        auto begin = csp::now() + scaled(0.01);
        for (auto const& move : moves) {
            chan<bool> done(1);
            MotionOrder order{MotionAction::move, phase, move.kind, cargo, id, seconds, begin, move.targets, std::move(done.w)};
            if (!(move.entity->commands << std::move(order))) throw std::runtime_error("animation imp stopped");
            completions.push_back(std::move(done.r));
        }
        state(phase.c_str(), cargo);
        for (auto& done : completions) {
            for (;;) {
                Signal signal; bool complete = false;
                int selected = prialt(control >> signal, done >> complete);
                if (selected == 0) handle(signal);
                else if (selected == 1 && complete) break;
                else throw std::runtime_error("animation did not complete");
            }
        }
    }
    void interrupt(Entity& entity, MotionAction action) {
        if (!entity.commands) return;
        chan<bool> done(1);
        MotionOrder order; order.action = action; order.done = std::move(done.w);
        if (entity.commands << std::move(order)) { bool ok; done.r >> ok; }
        if (action == MotionAction::stop) entity.commands = {};
    }
};

Point parking(int berth) { return {-620.0 - (berth / 3) * 100, -130.0 + (berth % 3) * 125, 22}; }
Point rack(int slot) { return {-80.0 + (slot % 6) * 22, -100.0 - (slot / 6) * 25, 8}; }
Point platform(int slot) { return {111.0 + slot * 30, 228, 12}; }

std::array<writer<Signal>, actor_count> start(writer<Event> events, double speed, time_point epoch) {
    chan<Cargo> arrivals(approach_capacity), storage(warehouse_capacity), fabrication, departures(tram_capacity);
    chan<int> berths(9), racks(11), platforms(5), intake_lease(1);
    for (int i = 0; i < 9; ++i) berths.w << i;
    for (int i = 0; i < 11; ++i) racks.w << i;
    for (int i = 0; i < 5; ++i) platforms.w << i;
    intake_lease.w << 0;
    auto intake = make_entity("door:hold-in", "door", {}, 3, 0, events.copy(), epoch, speed);
    std::array<writer<Signal>, actor_count> controls;
    std::array<reader<Signal>, actor_count> inputs;
    for (int i = 0; i < actor_count; ++i) {
        chan<Signal> control(8);
        controls[i] = std::move(control.w); inputs[i] = std::move(control.r);
    }
    auto launch = [&](int id, auto work) {
        spawn([actor = Actor{id, speed, epoch, std::move(inputs[id]), events.copy()}, work = std::move(work)]() mutable {
            try { work(actor); }
            catch (...) { actor.report(Kind::violation, 0, "worker failed"); }
            actor.report(Kind::finished);
        });
    };
    launch(0, [out = std::move(arrivals.w), slots = std::move(berths.r)](Actor& a) mutable {
        int serial = 0;
        while (!a.stopping) {
            a.state("inbound guidance"); a.delay(a.surge ? 0.20 : 2.90);
            if (a.stopping) break;
            Cargo cargo;
            if (!a.receive(slots, cargo.berth) || a.stopping) break;
            cargo.id = ++serial;
            auto target = parking(cargo.berth);
            Point origin{target.x - 260, target.y - 80, target.z + 60};
            cargo.box = a.entity("cargo:" + std::to_string(cargo.id), "raw", origin, cargo.id);
            cargo.ship = a.entity("ship:" + std::to_string(cargo.id), "ship", origin, cargo.id);
            a.report(Kind::created, cargo.id, "arrival");
            a.play("arrival", 0.75, cargo.id, {{&cargo.box, "raw", {target}}, {&cargo.ship, "ship", {target}}});
            int id = cargo.id;
            if (!a.send(out, cargo)) break;
            a.report(Kind::handoff, id);
        }
    });
    for (int id : {1, 2}) {
        launch(id, [in = arrivals.r.copy(), out = storage.w.copy(), returns = berths.w.copy(),
                    slots = racks.r.copy(), lease = intake_lease.r.copy(), release = intake_lease.w.copy(),
                    door = intake.copy()](Actor& a) mutable {
            auto rig = a.entity("crane:" + std::to_string(a.id), "rig", a.id == 1 ? Point{-425,-90,140} : Point{-330,200,140});
            std::optional<Cargo> held;
            int step = 0, token = 0;
            bool leased = false, slotted = false;
            auto policy = on_exit([&](imp_event event) {
                if (!event.error) return;
                try { std::rethrow_exception(event.error); }
                catch (CraneFault const&) {
                    a.interrupt(rig, MotionAction::freeze);
                    if (held) { a.interrupt(held->box, MotionAction::freeze); a.interrupt(held->ship, MotionAction::freeze); }
                    if (leased) a.interrupt(door, MotionAction::freeze);
                    a.report(Kind::restarted, 0, "controller restarting");
                    event.restart(a.scaled(1.8));
                }
                catch (...) { a.report(Kind::violation, 0, "unexpected controller failure"); }
            });
            supervised([&] {
                for (;;) {
                    if (!held) { Cargo cargo; if (!a.receive(in, cargo)) return; held = std::move(cargo); step = 0; slotted = false; }
                    auto& c = *held;
                    Point berth = a.id == 1 ? Point{-425,-90,22} : Point{-330,200,22};
                    Point high{berth.x, berth.y, 112};
                    Point drop = a.id == 1 ? Point{-225,-2,8} : Point{-185,120,8};
                    if (step == 0) { a.play("berth", 0.65, c.id, {{&c.box,"raw",{berth}}, {&c.ship,"ship",{berth}}, {&rig,"rig",{{berth.x,berth.y,140}}}}); ++step; }
                    if (step == 1) { a.play("hook-lower", 0.28, c.id, {{&rig,"rig",{berth}}}); ++step; }
                    if (step == 2) { a.play("crane-lift", 0.55, c.id, {{&rig,"rig",{high}}, {&c.box,"raw",{high}}}); ++step; }
                    if (step == 3) {
                        Point middle = a.id == 1 ? Point{-350,-140,128} : Point{-270,102,128};
                        a.play("crane-slew", 0.70, c.id, {{&rig,"rig",{middle,{drop.x,drop.y,112}}}, {&c.box,"raw",{middle,{drop.x,drop.y,112}}}, {&c.ship,"ship",{{berth.x-480,berth.y-100,100}}}});
                        a.interrupt(c.ship, MotionAction::stop); returns << c.berth; ++step;
                    }
                    if (step == 4) { a.play("crane-lower", 0.40, c.id, {{&rig,"rig",{drop}}, {&c.box,"raw",{drop}}}); ++step; }
                    if (!slotted) { if (!a.receive(slots, c.rack, false)) return; slotted = true; }
                    if (step <= 8 && !leased) { if (!a.receive(lease, token, false)) return; leased = true; }
                    if (step == 5) { a.play("road-to-hold", 0.85, c.id, {{&c.box,"raw",{{-148,52,8},{-148,-196,8},{-50,-180,8}}}, {&rig,"rig",{{drop.x,drop.y,140}}}}); ++step; }
                    if (step == 6) { a.play("hold-in-open", 0.18, c.id, {{&door,"door",{{1,0,0}}}}); ++step; }
                    if (step == 7) { a.play("hold-intake", 0.35, c.id, {{&c.box,"raw",{rack(c.rack)}}}); ++step; }
                    if (step == 8) { a.play("stored", 0.18, c.id, {{&c.box,"hidden",{rack(c.rack)}}, {&door,"door",{{0,0,0}}}}); ++step; }
                    if (leased) { release << token; leased = false; }
                    int serial = c.id;
                    if (!a.send(out, c)) return;
                    a.report(Kind::handoff, serial);
                    held.reset();
                }
            })();
        });
    }
    launch(3, [in = std::move(storage.r), out = std::move(fabrication.w), returns = std::move(racks.w)](Actor& a) mutable {
        auto door = a.entity("door:hold", "door", {});
        Cargo c;
        while (a.receive(in, c)) {
            a.play("hold-door-open", 0.22, c.id, {{&door,"door",{{1,0,0}}}});
            a.play("warehouse-out", 0.55, c.id, {{&c.box,"raw",{{-49,-60,8},{-49,-12,8}}}});
            returns << c.rack;
            a.play("to-fabricator", 0.75, c.id, {{&c.box,"raw",{{-9,12,8},{120,12,8},{175,-23,8}}}, {&door,"door",{{0,0,0}}}});
            int serial = c.id;
            if (!a.send(out, c)) break;
            a.report(Kind::handoff, serial);
        }
    });
    launch(4, [in = std::move(fabrication.r), out = std::move(departures.w), slots = std::move(platforms.r)](Actor& a) mutable {
        auto inlet = a.entity("door:fab-in", "door", {});
        auto outlet = a.entity("door:fab-out", "door", {});
        auto effect = a.entity("factory:4", "effect", {});
        Cargo c;
        while (a.receive(in, c)) {
            a.play("fab-door-open", 0.20, c.id, {{&inlet,"door",{{1,0,0}}}});
            a.play("fab-intake", 0.35, c.id, {{&c.box,"raw",{{175,-77,8}}}});
            a.play("processing", 0.65, c.id, {{&c.box,"hidden",{{175,-77,8}}}, {&inlet,"door",{{0,0,0}}}, {&effect,"effect",{{1,0,0}}}});
            a.play("conversion", 0.35, c.id, {{&c.box,"hidden",{{270,-77,8}}}, {&effect,"effect",{{2,0,0}}}});
            a.play("fab-out-open", 0.20, c.id, {{&outlet,"door",{{1,0,0}}}});
            a.play("fab-out", 0.40, c.id, {{&c.box,"goods",{{284,-10,8}}}});
            if (!a.receive(slots, c.platform, false)) break;
            a.play("conveyor", 0.90, c.id, {{&c.box,"goods",{{330,30,8},{330,165,8},{260,205,12},platform(c.platform)}}, {&outlet,"door",{{0,0,0}}}, {&effect,"effect",{{0,0,0}}}});
            a.play("platform", 0.12, c.id, {{&c.box,"goods",{platform(c.platform)}}});
            int serial = c.id;
            if (!a.send(out, c)) break;
            a.report(Kind::handoff, serial);
        }
    });
    launch(5, [in = std::move(departures.r), returns = std::move(platforms.w)](Actor& a) mutable {
        auto tram = a.entity("tram:5", "tram", {165,276,0});
        Cargo c;
        while (a.receive(in, c)) {
            auto load = platform(c.platform);
            a.play("loading", 0.55, c.id, {{&c.box,"goods",{{load.x,244,48},{165,283,33}}}});
            returns << c.platform;
            a.play("tram-ride", 1.0, c.id, {{&tram,"tram",{{398,276,0}}}, {&c.box,"goods",{{398,283,33}}}});
            a.play("habitat-unload", 0.50, c.id, {{&c.box,"goods",{{420,270,50},{430,237,50}}}});
            a.interrupt(c.box, MotionAction::stop);
            a.report(Kind::delivered, c.id, "habitat");
            a.play("tram-return", 0.75, c.id, {{&tram,"tram",{{165,276,0}}}});
        }
    });
    return controls;
}

void coordinate(reader<Query> requests, double speed) {
    chan<Event> events(event_capacity);
    Snapshot state;
    state.run = 1;
    auto epoch = csp::now();
    auto controls = start(events.w.copy(), speed, epoch);
    double last_fault = -10;
    bool quitting = false;
    std::array<bool, actor_count> pending_evac{};
    auto signal = [&](int actor, Signal value) { return prialt(controls[actor] << value, csp::none) == 0; };
    auto seconds = [&] { return std::chrono::duration<double>(csp::now() - epoch).count() * speed; };
    auto note = [&](int actor, std::string text) {
        state.notes.push_front({seconds(), actor, std::move(text)});
        if (state.notes.size() > history_limit) state.notes.pop_back();
    };
    auto evacuate = [&] {
        state.mode = "draining";
        state.bay_closed = state.factory_paused = false;
        pending_evac.fill(true);
        note(0, "Evacuation: arrivals stopped; accepted cargo is draining");
    };
    note(0, "Dock online. Every moving cargo is a real channel handoff.");
    for (;;) {
        bool pending = false;
        for (int i = 0; i < actor_count; ++i) if (pending_evac[i]) {
            if (prialt(controls[i] << Signal::evacuate, csp::none) != csp::none) pending_evac[i] = false;
            else pending = true;
        }
        Query query;
        Event event;
        int selected;
        if (quitting) {
            selected = pending ? prialt(events.r >> event, after(10ms) >> nullptr)
                               : ((events.r >> event) ? 0 : -1);
            if (selected == 1) continue;
        } else selected = pending ? prialt(events.r >> event, requests >> query, after(10ms) >> nullptr)
                                  : prialt(events.r >> event, requests >> query);
        if (selected == ~1) {
            quitting = true;
            if (state.mode == "running") evacuate();
        } else if (selected == 0) {
            auto& actor = state.actors[event.actor];
            switch (event.kind) {
            case Kind::created:
                if (event.cargo != state.created + 1) ++state.violations;
                ++state.created;
                state.items.emplace(event.cargo, Item{event.cargo, event.actor, "arrival", "space", seconds(), 0});
                break;
            case Kind::handoff: ++state.transfers; break;
            case Kind::delivered:
                if (state.items.erase(event.cargo) != 1) ++state.violations;
                else ++state.delivered;
                note(event.actor, "Habitat received finished supplies " + std::to_string(event.cargo));
                break;
            case Kind::status: actor.status = event.stage; actor.cargo = event.cargo; break;
            case Kind::finished: actor.status = "offline"; actor.cargo = 0; --state.active; break;
            case Kind::restarted: ++state.restarts; actor.status = "restarting"; note(event.actor, "Controller failed; its motion imps hold their poses until restart"); break;
            case Kind::violation: ++state.violations; note(event.actor, event.stage); break;
            case Kind::entity_started: ++state.animation_active; [[fallthrough]];
            case Kind::motion: {
                auto& motion = event.motion;
                auto old = state.motions.find(motion.key);
                if (old != state.motions.end()) {
                    auto p = interpolate(old->second, motion.at);
                    auto q = motion.frames.front().p;
                    if (std::hypot(p.x-q.x, p.y-q.y, p.z-q.z) > 0.01) ++state.motion_violations;
                }
                if (motion.frames.size() < 2 || motion.frames.front().t != 0 || motion.frames.back().t != 1 || motion.duration < 0) ++state.motion_violations;
                motion.revision = ++state.motion_serial;
                if (motion.key.starts_with("cargo:")) {
                    auto item = state.items.find(motion.cargo);
                    if (item != state.items.end()) item->second = {motion.cargo, motion.actor, motion.phase, item->second.stage, motion.at, motion.duration};
                }
                auto key = motion.key;
                state.motions[key] = std::move(motion);
                break;
            }
            case Kind::motion_done: ++state.motion_completed; break;
            case Kind::entity_stopped:
                state.motions.erase(event.stage); ++state.motion_serial; --state.animation_active;
                break;
            }
            if (state.created != state.delivered + static_cast<int>(state.items.size()) || state.items.size() > cargo_limit) ++state.violations;
            if (state.active == 0 && state.animation_active == 0 && state.mode != "evacuated") {
                if (!state.items.empty()) ++state.violations;
                state.mode = state.violations || state.motion_violations ? "failed" : "evacuated";
                note(0, state.mode == "failed" ? "Dock stopped with an invariant failure."
                                              : "All supplies delivered. Logistics and animation imps have stopped.");
            }
        } else if (selected == 1) {
            bool ok = true;
            switch (query.command) {
            case Command::snapshot: break;
            case Command::reset:
                if (state.active || state.animation_active) ok = false;
                else {
                    int run = state.run + 1;
                    state = Snapshot{}; state.run = run; epoch = csp::now(); last_fault = -10;
                    controls = start(events.w.copy(), speed, epoch);
                    pending_evac.fill(false);
                    note(0, "A new shift is online");
                }
                break;
            case Command::quit: quitting = true; [[fallthrough]];
            case Command::evacuate: if (state.mode == "running") evacuate(); break;
            default:
                if (state.mode != "running") { ok = false; break; }
                if (query.command == Command::bay) {
                    if (!signal(1, Signal::toggle)) { ok = false; break; }
                    state.bay_closed = !state.bay_closed;
                    note(1, state.bay_closed ? "Aster bay closed. Boreal takes incoming traffic." : "Aster bay reopened");
                } else if (query.command == Command::factory) {
                    if (!signal(4, Signal::toggle)) { ok = false; break; }
                    state.factory_paused = !state.factory_paused;
                    note(4, state.factory_paused ? "Fabricator paused. Storage will fill upstream." : "Fabricator resumed. The backlog can drain.");
                } else if (query.command == Command::surge) {
                    if (!signal(0, Signal::surge)) { ok = false; break; }
                    state.surge = !state.surge;
                    note(0, state.surge ? "Traffic surge: arrivals increased" : "Arrival rate returned to normal");
                } else if (query.command == Command::fault) {
                    if (seconds() - last_fault < 3) ok = false;
                    else if (signal(1, Signal::fault)) last_fault = seconds();
                    else ok = false;
                }
            }
            state.now = seconds();
            // Every request supplies a capacity-one mailbox. Sending a copy
            // never waits for a renderer, socket or subsequent browser poll.
            query.reply << Reply{state, ok};
        }
        if (quitting && !state.active && !state.animation_active) return;
    }
}

Reply ask(writer<Query>& channel, Command command) {
    chan<Reply> mailbox(1);
    if (!(channel << Query{command, std::move(mailbox.w)})) throw std::runtime_error("simulation stopped");
    Reply reply;
    if (!(mailbox.r >> reply)) throw std::runtime_error("simulation reply missing");
    return reply;
}

std::string quote(std::string const& text) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
std::string json(Reply const& reply) {
    auto const& s = reply.state;
    std::ostringstream out;
    out << "{\"ok\":" << (reply.ok ? "true" : "false") << ",\"run\":" << s.run
        << ",\"now\":" << s.now << ",\"mode\":" << quote(s.mode)
        << ",\"created\":" << s.created << ",\"delivered\":" << s.delivered
        << ",\"inFlight\":" << s.items.size() << ",\"activeActors\":" << s.active
        << ",\"transfers\":" << s.transfers << ",\"restarts\":" << s.restarts
        << ",\"animationActors\":" << s.animation_active << ",\"motionCompleted\":" << s.motion_completed
        << ",\"motionViolations\":" << s.motion_violations << ",\"motionRevision\":" << s.motion_serial
        << ",\"violations\":" << s.violations
        << ",\"bayClosed\":" << (s.bay_closed ? "true" : "false")
        << ",\"factoryPaused\":" << (s.factory_paused ? "true" : "false")
        << ",\"surge\":" << (s.surge ? "true" : "false") << ",\"actors\":[";
    for (int i = 0; i < actor_count; ++i) {
        if (i) out << ',';
        out << "{\"id\":" << i << ",\"name\":" << quote(names[i]) << ",\"status\":" << quote(s.actors[i].status) << ",\"cargo\":" << s.actors[i].cargo << '}';
    }
    out << "],\"cargo\":[";
    bool comma = false;
    for (auto const& [id, item] : s.items) {
        if (comma) out << ',';
        comma = true;
        out << "{\"id\":" << id << ",\"type\":" << id % 3 << ",\"actor\":" << item.actor
            << ",\"stage\":" << quote(item.stage) << ",\"from\":" << quote(item.from)
            << ",\"at\":" << item.at << ",\"duration\":" << item.duration << '}';
    }
    out << "],\"events\":["; comma = false;
    for (auto const& event : s.notes) {
        if (comma) out << ',';
        comma = true;
        out << "{\"at\":" << event.at << ",\"actor\":" << event.actor << ",\"text\":" << quote(event.text) << '}';
    }
    return out.str() + "]}";
}
} // namespace lifeboat

#include "stream.h"

namespace lifeboat {
bool self_test(int workers) {
    bool passed = true;
    set_maxprocs(workers);
    chan<Query> requests(32);
    spawn([r = std::move(requests.r)]() mutable { coordinate(std::move(r), 40); });
    spawn([w = std::move(requests.w), &passed]() mutable {
        auto check = [&](bool condition, const char* property) {
            if (!condition) { passed = false; std::cout << "FAIL: " << property << '\n'; }
        };
        auto wait_for = [&](auto predicate) {
            Reply reply;
            for (int i = 0; i < 5000; ++i) {
                reply = ask(w, Command::snapshot);
                if (predicate(reply.state)) return reply.state;
                csp::sleep(5ms);
            }
            check(false, "scenario reached its bounded deadline");
            return reply.state;
        };
        wait_for([](auto const& s) { return s.delivered >= 8; });
        ask(w, Command::bay);
        ask(w, Command::factory);
        ask(w, Command::surge);
        auto congested = wait_for([](auto const& s) { return s.items.size() >= warehouse_capacity; });
        check(congested.violations == 0, "congestion conserves cargo");
        int before = congested.delivered;
        ask(w, Command::factory);
        ask(w, Command::bay);
        ask(w, Command::fault);
        wait_for([&](auto const& s) { return s.restarts >= 1 && s.delivered > before + 4; });
        // Control producers may outrun a worker. Saturation must reject a
        // command instead of stopping the coordinator from draining events.
        for (int i = 0; i < 256; ++i) ask(w, Command::bay);
        auto flooded = ask(w, Command::snapshot).state;
        check(flooded.active == actor_count && flooded.violations == 0,
              "control flooding leaves telemetry and workers responsive");
        // A viewer can abandon a full reply mailbox without stalling workers.
        { chan<Reply> abandoned(1); w << Query{Command::snapshot, std::move(abandoned.w)}; }
        ask(w, Command::evacuate);
        auto drained = wait_for([](auto const& s) { return s.active == 0 && s.animation_active == 0; });
        check(drained.created == drained.delivered && drained.items.empty(), "evacuation drains every cargo exactly once");
        check(drained.motion_violations == 0, "every motion starts at its previous pose");
        check(drained.motions.empty() && drained.motion_completed > drained.created * 15, "every visual twin finishes its choreography and exits");
        check(drained.violations == 0, "ownership and bounded population invariants hold");
        check(drained.transfers == drained.created * 4, "each delivered cargo completed exactly four channel sends");
        check(drained.restarts == 1, "the real CSP supervisor restarted the injected failure");
        check(ask(w, Command::reset).ok, "a drained dock can start a new shift");
        wait_for([](auto const& s) { return s.delivered >= 3; });
        ask(w, Command::quit);
    });
    await_completion();
    shutdown_runtime();
    if (passed) std::cout << "PASS: live CSP flow, congestion, recovery, supervision, abandoned viewer, evacuation and restart (" << workers << " workers)\n";
    return passed;
}

std::string read_asset(std::string const& directory, const char* name) {
    std::ifstream input(directory + '/' + name, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read Lifeboat web assets; use --assets PATH");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void serve(int port, std::string const& host, std::string const& assets) {
    // Explicit routes only: no request path is ever opened on the filesystem.
    const std::map<std::string, std::pair<std::string, std::string>> files = {
        {"/", {"text/html; charset=utf-8", read_asset(assets, "index.html")}},
        {"/app.js", {"text/javascript; charset=utf-8", read_asset(assets, "app.js")}},
        {"/style.css", {"text/css; charset=utf-8", read_asset(assets, "style.css")}}};
    set_maxprocs(4);
    chan<Query> requests(32);
    spawn([r = std::move(requests.r)]() mutable { coordinate(std::move(r), 1); });
    spawn([port, host, w = std::move(requests.w), &files] {
        auto server = http::serve(host, static_cast<uint16_t>(port));
        std::cout << "Lifeboat is running at http://" << host << ':' << server.port << "/\n" << std::flush;
        http::endpoint endpoint;
        while (server.endpoints >> endpoint) {
            spawn([endpoint = std::move(endpoint), w = w.copy(), &files]() mutable {
                http::request request;
                while (endpoint.requests >> request) {
                    if (request.method == http::method::GET && request.url == "/api/stream") {
                        try {
                            stream_scene(ws::upgrade(request, {.max_message_size = stream_ack_limit}), w.copy());
                        } catch (csp::error const&) {
                            // upgrade() sends the 400 response for bad handshakes.
                        }
                        return;
                    }
                    http::response response;
                    std::string body, type = "application/json";
                    if (request.method == http::method::GET && request.url == "/api/state") body = json(ask(w, Command::snapshot));
                    else if (request.method == http::method::POST && request.header("X-Lifeboat-Control") == "1" && request.content_length() <= 0) {
                        static const std::map<std::string, Command> commands = {
                            {"/api/bay", Command::bay}, {"/api/factory", Command::factory}, {"/api/fault", Command::fault},
                            {"/api/surge", Command::surge}, {"/api/evacuate", Command::evacuate}, {"/api/reset", Command::reset}};
                        auto command = commands.find(request.url);
                        if (command == commands.end()) response.status = 404;
                        else { auto reply = ask(w, command->second); body = json(reply); if (!reply.ok) response.status = 409; }
                    } else if (request.method == http::method::GET) {
                        auto asset = files.find(request.url);
                        if (asset == files.end()) response.status = 404;
                        else { type = asset->second.first; body = asset->second.second; }
                    } else response.status = 400;
                    response.headers = {{"Content-Type", type}, {"Cache-Control", "no-store"}, {"X-Content-Type-Options", "nosniff"},
                        {"Content-Security-Policy", "default-src 'self'; style-src 'self'; script-src 'self'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'"}};
                    response.body = bytes(body.begin(), body.end());
                    request.respond << std::move(response);
                }
            });
        }
    });
    await_completion();
}
} // namespace lifeboat

int main(int argc, char** argv) {
    int port = 8042, workers = 4;
    bool test = false;
    std::string host = "127.0.0.1", assets = "demos/lifeboat/web";
    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "--help-agent") {
                std::cout << "Lifeboat — an orbital dock orchestrated by CSP\n"
                    "Run from the CSP root: make lifeboat && build/lifeboat/lifeboat\n"
                    "Options: --port 8042 --host 127.0.0.1 --assets demos/lifeboat/web\n"
                    "         --self-test --workers 4 | --version | --help-agent\n"
                    "GET /api/stream streams motion twins over WebSocket; ACK each decimal seq.\n"
                    "GET /api/state returns telemetry. POST /api/{bay,factory,fault,surge,evacuate,reset}\n"
                    "with header X-Lifeboat-Control: 1 changes the simulation. No request body.\n";
                return 0;
            }
            if (arg == "--version") { std::cout << "Lifeboat / CSP " << CSP_VERSION << '\n'; return 0; }
            if (arg == "--self-test") { test = true; continue; }
            if (++i == argc) throw std::runtime_error("option requires a value");
            std::string value = argv[i];
            if (arg == "--host") host = value;
            else if (arg == "--assets") assets = value;
            else if (arg == "--port" || arg == "--workers") {
                int number = 0;
                auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
                if (error != std::errc{} || end != value.data() + value.size() || number < 1 || number > (arg == "--port" ? 65535 : 64)) throw std::runtime_error("invalid numeric option");
                (arg == "--port" ? port : workers) = number;
            } else throw std::runtime_error("unknown option");
        }
        if (test) return lifeboat::self_test(workers) ? 0 : 1;
        lifeboat::serve(port, host, assets);
        return 0;
    } catch (std::exception const& error) {
        std::cerr << "Lifeboat: " << error.what() << '\n';
        return 1;
    }
}
