// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
// The picture observes this application. Every cargo handoff below is a real
// CSP channel operation; the browser never advances the simulation.
#include "csp.h"

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
enum class Kind { created, moved, handoff, status, delivered, finished, restarted, violation };
struct Cargo { int id = 0; };
struct Event {
    Kind kind;
    int actor;
    Cargo cargo{};
    std::string stage;
    double duration = 0;
};
struct Item { int id; int actor; std::string stage, from; double at, duration; };
struct ActorState { std::string status = "starting"; int cargo = 0; };
struct Note { double at; int actor; std::string text; };
struct Snapshot {
    int run = 0, created = 0, delivered = 0, active = actor_count;
    int transfers = 0, restarts = 0, violations = 0;
    bool bay_closed = false, factory_paused = false, surge = false;
    double now = 0;
    std::string mode = "running";
    std::array<ActorState, actor_count> actors;
    std::map<int, Item> items;
    std::deque<Note> notes;
};
struct Reply { Snapshot state; bool ok = true; };
struct Query { Command command; writer<Reply> reply; };
struct CraneFault {};

// A worker owns its control state. Control remains selectable even while a
// downstream channel is full. Closing a bay finishes its current delivery.
struct Actor {
    int id;
    double speed;
    reader<Signal> control;
    writer<Event> events;
    bool paused = false, stopping = false, surge = false;
    duration scaled(double seconds) const {
        return std::chrono::duration_cast<duration>(std::chrono::duration<double>(seconds / speed));
    }
    void report(Kind kind, Cargo cargo = {}, std::string stage = {}, double seconds = 0) {
        events << Event{kind, id, cargo, std::move(stage), seconds};
    }
    void state(const char* value, Cargo cargo = {}) { report(Kind::status, cargo, value); }
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
    bool receive(reader<Cargo>& input, Cargo& cargo) {
        for (;;) {
            ready();
            state("awaiting cargo");
            Signal signal;
            int choice = prialt(control >> signal, input >> cargo);
            if (choice == 0) handle(signal);
            else return choice == 1;
        }
    }
    bool send(writer<Cargo>& output, Cargo cargo) {
        state("backpressure", cargo);
        for (;;) {
            Signal signal;
            int choice = prialt(control >> signal, output << cargo);
            if (choice == 0) handle(signal);
            else return choice == 1;
        }
    }
};

// Only the coordinator owns Snapshot. Worker event streams are lossless;
// browser replies use single-slot mailboxes, so slow viewers do not block it.
std::array<writer<Signal>, actor_count> start(writer<Event> events, double speed) {
    chan<Cargo> arrivals(approach_capacity), storage(warehouse_capacity), fabrication, departures(tram_capacity);
    std::array<writer<Signal>, actor_count> controls;
    std::array<reader<Signal>, actor_count> inputs;
    for (int i = 0; i < actor_count; ++i) {
        chan<Signal> control(8);
        controls[i] = std::move(control.w);
        inputs[i] = std::move(control.r);
    }
    auto launch = [&](int id, auto work) {
        spawn([actor = Actor{id, speed, std::move(inputs[id]), events.copy()}, work = std::move(work)]() mutable {
            try { work(actor); }
            catch (...) { actor.report(Kind::violation, {}, "worker failed"); }
            actor.report(Kind::finished);
        });
    };
    launch(0, [out = std::move(arrivals.w)](Actor& a) mutable {
        int serial = 0;
        while (!a.stopping) {
            a.state("inbound guidance");
            a.delay(a.surge ? 0.16 : 0.60);
            if (a.stopping) break;
            Cargo cargo{++serial};
            a.report(Kind::created, cargo, "approach", 0.60);
            // An admitted cargo is always handed off, even after evacuation.
            if (!a.send(out, cargo)) break;
            a.report(Kind::handoff, cargo, "approach");
        }
    });
    for (int id : {1, 2}) {
        launch(id, [in = arrivals.r.copy(), out = storage.w.copy()](Actor& a) mutable {
            std::optional<Cargo> held;
            // The durable slot survives a failed worker invocation. Restart
            // cannot lose a cargo or start a duplicate delivery.
            auto policy = on_exit([&a](imp_event event) {
                if (!event.error) return;
                try { std::rethrow_exception(event.error); }
                catch (CraneFault const&) {
                    a.report(Kind::restarted, {}, "controller restarting");
                    event.restart(a.scaled(1.8));
                }
                catch (...) { a.report(Kind::violation, {}, "unexpected controller failure"); }
            });
            supervised([&] {
                for (;;) {
                    if (!held) {
                        Cargo cargo;
                        if (!a.receive(in, cargo)) return;
                        held = cargo;
                    }
                    a.report(Kind::moved, *held, a.id == 1 ? "craneA" : "craneB", 1.15);
                    a.state("lifting", *held);
                    a.delay(1.15);
                    if (!a.send(out, *held)) return;
                    a.report(Kind::handoff, *held, "warehouse", 0.35);
                    held.reset();
                }
            })();
        });
    }
    launch(3, [in = std::move(storage.r), out = std::move(fabrication.w)](Actor& a) mutable {
        Cargo cargo;
        while (a.receive(in, cargo)) {
            if (!a.send(out, cargo)) break;
            a.report(Kind::handoff, cargo, "factory");
        }
    });
    launch(4, [in = std::move(fabrication.r), out = std::move(departures.w)](Actor& a) mutable {
        Cargo cargo;
        while (a.receive(in, cargo)) {
            a.report(Kind::moved, cargo, "factory", 0.48);
            a.state("fabricating", cargo);
            a.delay(0.48);
            if (!a.send(out, cargo)) break;
            a.report(Kind::handoff, cargo, "platform", 0.25);
        }
    });
    launch(5, [in = std::move(departures.r)](Actor& a) mutable {
        Cargo cargo;
        while (a.receive(in, cargo)) {
            a.report(Kind::moved, cargo, "tram", 0.52);
            a.state("delivering", cargo);
            a.delay(0.52);
            a.report(Kind::delivered, cargo, "habitat");
        }
    });
    return controls;
}

void coordinate(reader<Query> requests, double speed) {
    chan<Event> events(event_capacity);
    Snapshot state;
    state.run = 1;
    auto controls = start(events.w.copy(), speed);
    auto epoch = csp::now();
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
    auto rank = [](std::string const& stage) {
        if (stage == "approach") return 0;
        if (stage == "craneA" || stage == "craneB") return 1;
        if (stage == "warehouse") return 2;
        if (stage == "factory") return 3;
        if (stage == "platform") return 4;
        if (stage == "tram") return 5;
        return -1;
    };
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
                if (event.cargo.id != state.created + 1) ++state.violations;
                ++state.created;
                state.items.emplace(event.cargo.id, Item{event.cargo.id, event.actor, "approach", "space", seconds(), event.duration});
                break;
            case Kind::moved:
            case Kind::handoff: {
                // Completion notifications can arrive after a receiver's next
                // stage. Count the committed send once, but never rewind cargo.
                if (event.kind == Kind::handoff) ++state.transfers;
                auto found = state.items.find(event.cargo.id);
                if (found == state.items.end()) {
                    if (event.kind != Kind::handoff) ++state.violations;
                    break;
                }
                auto previous = found->second.stage;
                if (rank(event.stage) < rank(previous) ||
                    (event.kind == Kind::handoff && rank(event.stage) == rank(previous))) break;
                int destination = event.actor;
                if (event.kind == Kind::handoff) {
                    if (event.stage == "warehouse") destination = 3;
                    if (event.stage == "factory") destination = 4;
                    if (event.stage == "platform") destination = 5;
                }
                found->second = {event.cargo.id, destination, event.stage, previous, seconds(), event.duration};
                break;
            }
            case Kind::delivered:
                if (state.items.erase(event.cargo.id) != 1) ++state.violations;
                else ++state.delivered;
                if (state.delivered % 8 == 0) note(event.actor, "Habitat received cargo " + std::to_string(event.cargo.id));
                break;
            case Kind::status: actor.status = event.stage; actor.cargo = event.cargo.id; break;
            case Kind::finished: actor.status = "offline"; actor.cargo = 0; --state.active; break;
            case Kind::restarted: ++state.restarts; actor.status = "restarting"; note(event.actor, "Controller failed; supervisor is restarting its work"); break;
            case Kind::violation: ++state.violations; note(event.actor, event.stage); break;
            }
            if (state.created != state.delivered + static_cast<int>(state.items.size()) || state.items.size() > cargo_limit) ++state.violations;
            if (state.active == 0 && state.mode != "evacuated") {
                if (!state.items.empty()) ++state.violations;
                state.mode = state.violations ? "failed" : "evacuated";
                note(0, state.violations ? "Dock stopped with an invariant failure; inspect cargo and diagnostics."
                                       : "Dock evacuated. All accepted cargo delivered; all six actors stopped.");
            }
        } else if (selected == 1) {
            bool ok = true;
            switch (query.command) {
            case Command::snapshot: break;
            case Command::reset:
                if (state.active) ok = false;
                else {
                    int run = state.run + 1;
                    state = Snapshot{}; state.run = run; epoch = csp::now(); last_fault = -10;
                    controls = start(events.w.copy(), speed);
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
        if (quitting && !state.active) return;
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
        if (comma) out << ','; comma = true;
        out << "{\"id\":" << id << ",\"type\":" << id % 3 << ",\"actor\":" << item.actor
            << ",\"stage\":" << quote(item.stage) << ",\"from\":" << quote(item.from)
            << ",\"at\":" << item.at << ",\"duration\":" << item.duration << '}';
    }
    out << "],\"events\":["; comma = false;
    for (auto const& event : s.notes) {
        if (comma) out << ','; comma = true;
        out << "{\"at\":" << event.at << ",\"actor\":" << event.actor << ",\"text\":" << quote(event.text) << '}';
    }
    return out.str() + "]}";
}

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
            for (int i = 0; i < 1000; ++i) {
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
        auto drained = wait_for([](auto const& s) { return s.active == 0; });
        check(drained.created == drained.delivered && drained.items.empty(), "evacuation drains every cargo exactly once");
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
                    "GET /api/state returns the live snapshot. POST /api/{bay,factory,fault,surge,evacuate,reset}\n"
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
