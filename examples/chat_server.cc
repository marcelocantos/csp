// chat_server.cc — Multi-room TCP chat server
//
// A real TCP chat server demonstrating CSP's I/O, channels, dynamic
// prialt, cancellation, and supervision. Connect with:
//   nc localhost 9000
//
// Protocol (line-based):
//   /nick <name>     — set nickname (default: "anon")
//   /join <room>     — join a room (default: "lobby")
//   /rooms           — list active rooms
//   /quit            — disconnect
//   <text>           — broadcast to current room
//
// Architecture:
//   - Accept loop spawns a supervised client imp per connection.
//   - Each room has a broadcast imp that fans messages to subscribers.
//   - Rooms are created on first /join and tracked in a shared map.
//   - SIGINT/SIGTERM triggers graceful shutdown via cancellation.

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "csp.h"

#include <arpa/inet.h>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace csp;
using namespace std::chrono_literals;

// Per-room state: a broadcast channel for messages and a registration
// channel for new subscribers. The broadcast imp reads from both and
// fans messages to all live subscribers.
struct Room {
    writer<std::string> broadcast;
    writer<writer<std::string>> subscribe;
};

// Spawn a broadcast imp that reads messages from `msgs` and fans them
// to all subscribers registered via `subs`. Dead subscribers are
// removed automatically.
static void spawn_broadcaster(reader<std::string> msgs,
                               reader<writer<std::string>> subs) {
    spawn([msgs = std::move(msgs), subs = std::move(subs)]() mutable {
        internal::descr("broadcaster");

        std::vector<writer<std::string>> outs;
        std::string msg;
        writer<std::string> new_out;

        for (;;) {
            switch (alt(msgs >> msg, subs >> new_out)) {
            case 0: // Broadcast message to all subscribers.
                for (auto it = outs.begin(); it != outs.end();) {
                    if (*it << msg) {
                        ++it;
                    } else {
                        // Dead subscriber — swap-remove.
                        *it = std::move(outs.back());
                        outs.pop_back();
                    }
                }
                break;
            case 1: // New subscriber.
                outs.push_back(std::move(new_out));
                break;
            case ~0: // Message channel closed — room is shutting down.
                return;
            case ~1: // No more subscriptions possible.
                // Keep broadcasting to existing subscribers.
                for (; msgs >> msg;) {
                    for (auto it = outs.begin(); it != outs.end();) {
                        if (*it << msg) {
                            ++it;
                        } else {
                            *it = std::move(outs.back());
                            outs.pop_back();
                        }
                    }
                    if (outs.empty()) return;
                }
                return;
            }
        }
    });
}

// Thread-safe room registry. Rooms are created on first join.
struct RoomRegistry {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<Room>> rooms;

    std::shared_ptr<Room> get_or_create(const std::string& name) {
        std::lock_guard lock(mu);
        auto it = rooms.find(name);
        if (it != rooms.end()) return it->second;

        // Create broadcast and subscription channels.
        auto [bc_w, bc_r] = chan<std::string>{};
        auto [sub_w, sub_r] = chan<writer<std::string>>{};

        // Spawn the broadcaster.
        spawn_broadcaster(std::move(bc_r), std::move(sub_r));

        auto room = std::make_shared<Room>(Room{
            std::move(bc_w),
            std::move(sub_w),
        });
        rooms.emplace(name, room);
        return room;
    }

    std::vector<std::string> list() {
        std::lock_guard lock(mu);
        std::vector<std::string> names;
        names.reserve(rooms.size());
        for (auto& [name, _] : rooms) names.push_back(name);
        return names;
    }
};

static void send_line(writer<std::vector<uint8_t>>& out, const std::string& msg) {
    std::string line = msg + "\n";
    std::vector<uint8_t> data(line.begin(), line.end());
    out << std::move(data);
}

static void handle_client(io::fd_t fd, std::shared_ptr<RoomRegistry> registry) {
    // Set up I/O pipelines.
    auto lines = part::io::split_lines.spawn(
                     part::io::byte_reader(fd).spawn());
    auto wfd = io::fd_t(::dup(fd.raw()));
    io::set_nonblock(wfd);
    auto out = part::io::byte_writer(wfd).spawn();

    std::string nick = "anon";
    std::string room_name = "lobby";

    // Subscribe to initial room.
    auto [my_w, my_r] = chan<std::string>{};
    auto room = registry->get_or_create(room_name);
    room->subscribe.copy() << std::move(my_w);

    // Announce arrival.
    room->broadcast.copy() << (nick + " joined " + room_name);

    send_line(out, "Welcome! You are in #" + room_name +
              ". Type /help for commands.");

    // Main loop: multiplex incoming lines and room messages.
    std::string line;
    std::string room_msg;
    for (;;) {
        switch (prialt(done(), lines >> line, my_r >> room_msg)) {
        case ~0:  // Cancelled (server shutting down).
            send_line(out, "Server shutting down. Goodbye!");
            return;

        case ~1:  // Client disconnected.
            room->broadcast.copy() << (nick + " left");
            return;

        case ~2:  // Room died (shouldn't happen normally).
            send_line(out, "Room closed.");
            return;

        case 1: {  // Incoming line from client.
            if (line.empty()) break;

            if (line[0] == '/') {
                // Parse command.
                auto space = line.find(' ');
                auto cmd = line.substr(0, space);
                auto arg = (space != std::string::npos)
                    ? line.substr(space + 1) : std::string{};

                if (cmd == "/nick") {
                    if (arg.empty()) {
                        send_line(out, "Usage: /nick <name>");
                    } else {
                        auto old = nick;
                        nick = arg;
                        room->broadcast.copy()
                            << (old + " is now " + nick);
                    }
                } else if (cmd == "/join") {
                    if (arg.empty()) {
                        send_line(out, "Usage: /join <room>");
                    } else {
                        // Leave current room.
                        room->broadcast.copy()
                            << (nick + " left " + room_name);
                        my_r = {};  // Unsubscribe (kills old writer).

                        // Join new room.
                        room_name = arg;
                        auto [w, r] = chan<std::string>{};
                        room = registry->get_or_create(room_name);
                        room->subscribe.copy() << std::move(w);
                        my_r = std::move(r);

                        room->broadcast.copy()
                            << (nick + " joined " + room_name);
                        send_line(out, "Joined #" + room_name);
                    }
                } else if (cmd == "/rooms") {
                    auto names = registry->list();
                    std::string msg = "Active rooms:";
                    for (auto& n : names) msg += " #" + n;
                    send_line(out, msg);
                } else if (cmd == "/quit") {
                    room->broadcast.copy() << (nick + " left");
                    send_line(out, "Goodbye!");
                    return;
                } else if (cmd == "/help") {
                    send_line(out, "Commands:");
                    send_line(out, "  /nick <name>  — set nickname");
                    send_line(out, "  /join <room>  — switch room");
                    send_line(out, "  /rooms        — list rooms");
                    send_line(out, "  /quit         — disconnect");
                } else {
                    send_line(out, "Unknown command: " + cmd);
                }
            } else {
                // Broadcast message to room.
                room->broadcast.copy()
                    << ("[" + nick + "] " + line);
            }
            break;
        }

        case 2:  // Message from room.
            send_line(out, room_msg);
            break;
        }
    }
}

int main() {
    constexpr uint16_t PORT = 9000;

    spawn([]{
        // Watch for SIGINT/SIGTERM. Created BEFORE the cancellation
        // scope so the signal producer imp (which uses io::read
        // internally) is not subject to cancellation — cancelling an
        // io::read on a fcontext stack triggers std::terminate.
        auto sig = signal::notify({SIGINT, SIGTERM});

        // Cancellation scope for graceful shutdown. Wrap in shared_ptr
        // so the signal imp can trigger it without a dangling reference
        // (in M:N mode, the outer imp may destroy its locals while the
        // signal imp is still executing guard->operator()()).
        auto guard = std::make_shared<cancel_guard>(cancellation());

        spawn([guard, sig = std::move(sig)]() mutable {
            int s;
            if (sig >> s) {
                fprintf(stderr, "\nReceived signal %d, shutting down...\n", s);
                (*guard)();  // Cancel the scope.
            }
        });

        auto registry = std::make_shared<RoomRegistry>();

        // Create the listen socket.
        io::fd_t listen_fd(::socket(AF_INET6, SOCK_STREAM, 0));
        if (!listen_fd) {
            fprintf(stderr, "socket: %s\n", strerror(errno));
            return;
        }

        int opt = 1;
        setsockopt(listen_fd.raw(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Dual-stack: accept both IPv4 and IPv6.
        int off = 0;
        setsockopt(listen_fd.raw(), IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(PORT);
        addr.sin6_addr = in6addr_any;

        if (::bind(listen_fd.raw(), reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0) {
            fprintf(stderr, "bind: %s\n", strerror(errno));
            io::close(listen_fd);
            return;
        }

        if (::listen(listen_fd.raw(), 128) < 0) {
            fprintf(stderr, "listen: %s\n", strerror(errno));
            io::close(listen_fd);
            return;
        }

        io::set_nonblock(listen_fd);
        fprintf(stderr, "Chat server listening on port %d\n", PORT);

        // Supervised accept loop: restart client imps on failure.
        auto exit_handler = on_exit(restart_policy{
            .max_restarts = 10, .window = 5s, .backoff = 100ms});

        // Accept loop. io::accept throws canceled on shutdown.
        for (;;) {
            sockaddr_storage client_addr{};
            socklen_t client_len = sizeof(client_addr);
            io::fd_t client_fd;
            try {
                client_fd = io::accept(
                    listen_fd,
                    reinterpret_cast<sockaddr*>(&client_addr),
                    &client_len);
            } catch (canceled const&) {
                break;  // Shutdown signal received.
            }

            if (!client_fd) continue;

            // Log connection.
            char addr_str[INET6_ADDRSTRLEN]{};
            if (client_addr.ss_family == AF_INET6) {
                auto* a = reinterpret_cast<sockaddr_in6*>(&client_addr);
                inet_ntop(AF_INET6, &a->sin6_addr,
                          addr_str, sizeof(addr_str));
            } else {
                auto* a = reinterpret_cast<sockaddr_in*>(&client_addr);
                inet_ntop(AF_INET, &a->sin_addr,
                          addr_str, sizeof(addr_str));
            }
            fprintf(stderr, "Client connected from %s\n", addr_str);

            spawn(supervised([client_fd, registry] {
                handle_client(client_fd, registry);
            }));
        }

        io::close(listen_fd);
        fprintf(stderr, "Server stopped.\n");
    });

    schedule();
}
