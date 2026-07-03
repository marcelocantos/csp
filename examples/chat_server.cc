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
//   - A single registry imp owns the room map and serves join / list
//     requests over request channels — no shared state, no mutex. Each
//     room broadcasts via the csp::part::fanout combinator (the same
//     dynamic pub/sub part chat_room.cc uses).
//   - Rooms are created on first /join.
//   - SIGINT/SIGTERM triggers graceful shutdown via cancellation.

// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "csp.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <map>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <variant>
#include <vector>

using namespace csp;
using namespace std::chrono_literals;

// Per-room state, held privately by the registry imp: the fanout's
// broadcast input (write a string to fan it to every subscriber) and its
// subscriber-registration channel (write a writer<string> to subscribe).
struct Room {
    writer<std::string>         broadcast;
    writer<writer<std::string>> subscribe;
};

// A join request: subscribe `subscriber` to room `name` and reply with the
// room's broadcast writer. The reply is a private per-client copy, so many
// clients can share the writer without stealing each other's replies.
struct JoinReq {
    std::string         name;
    writer<std::string> subscriber;
};

using JoinChan = writer<request<JoinReq, writer<std::string>>>;
using ListChan = writer<request<std::monostate, std::vector<std::string>>>;

// Registry imp: owns the room map and serves join / list requests serially.
// Because it is the sole owner, there is no lock and no create-race. On a
// join for a new room it spawns a fanout, registers the joining client as
// the first subscriber (which is what makes fanout emit its broadcast
// writer), and stashes both endpoints in the map.
static void spawn_registry(
    reader<request<JoinReq, writer<std::string>>>              join_r,
    reader<request<std::monostate, std::vector<std::string>>> list_r) {
    spawn([join_r = std::move(join_r), list_r = std::move(list_r)]() mutable {
        internal::descr("room-registry");

        std::map<std::string, Room> rooms;
        request<JoinReq, writer<std::string>>              jr;
        request<std::monostate, std::vector<std::string>>  lr;

        for (;;) {
            switch (alt(join_r >> jr, list_r >> lr)) {
            case 0: {  // Join (or create) a room.
                auto it = rooms.find(jr.value.name);
                if (it == rooms.end()) {
                    writer<writer<std::string>> sub_w;
                    auto bc_in = part::fanout<std::string>.spawn(--sub_w);
                    // First subscriber unblocks fanout's broadcast writer.
                    sub_w << std::move(jr.value.subscriber);
                    writer<std::string> bc_w;
                    bc_in >> bc_w;
                    bc_in = {};
                    it = rooms.emplace(jr.value.name,
                                       Room{std::move(bc_w), std::move(sub_w)})
                             .first;
                } else {
                    it->second.subscribe << std::move(jr.value.subscriber);
                }
                jr.reply << it->second.broadcast.copy();
                break;
            }
            case 1: {  // List active room names.
                std::vector<std::string> names;
                names.reserve(rooms.size());
                for (auto& [name, _] : rooms) names.push_back(name);
                lr.reply << std::move(names);
                break;
            }
            default:  // Either request channel closed — server shutting down.
                return;
            }
        }
    });
}

static void send_line(writer<std::vector<uint8_t>>& out, const std::string& msg) {
    std::string line = msg + "\n";
    std::vector<uint8_t> data(line.begin(), line.end());
    out << std::move(data);
}

static void handle_client(io::fd_t fd, JoinChan join_w, ListChan list_w) {
    // Set up I/O pipelines.
    auto lines = part::io::split_lines.spawn(
                     part::io::byte_reader(fd).spawn());
    auto wfd = io::fd_t(::dup(fd.raw()));
    io::set_nonblock(wfd);
    auto out = part::io::byte_writer(wfd).spawn();

    std::string nick = "anon";
    std::string room_name = "lobby";

    // Subscribe to the initial room; the reply is our broadcast writer.
    auto [my_w, my_r] = chan<std::string>{};
    writer<std::string> bcast = join_w(JoinReq{room_name, std::move(my_w)});

    // Announce arrival.
    bcast << (nick + " joined " + room_name);

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
            bcast << (nick + " left");
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
                        bcast << (old + " is now " + nick);
                    }
                } else if (cmd == "/join") {
                    if (arg.empty()) {
                        send_line(out, "Usage: /join <room>");
                    } else {
                        // Leave current room.
                        bcast << (nick + " left " + room_name);
                        my_r = {};  // Unsubscribe (kills old writer).

                        // Join new room.
                        room_name = arg;
                        auto [w, r] = chan<std::string>{};
                        bcast = join_w(JoinReq{room_name, std::move(w)});
                        my_r = std::move(r);

                        bcast << (nick + " joined " + room_name);
                        send_line(out, "Joined #" + room_name);
                    }
                } else if (cmd == "/rooms") {
                    auto names = list_w(std::monostate{});
                    std::string msg = "Active rooms:";
                    for (auto& n : names) msg += " #" + n;
                    send_line(out, msg);
                } else if (cmd == "/quit") {
                    bcast << (nick + " left");
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
                bcast << ("[" + nick + "] " + line);
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

        // Room registry imp. Clients reach it through copies of these
        // writers; when the accept loop and every client have dropped
        // their copies, the registry drains and exits.
        chan<request<JoinReq, writer<std::string>>>              join_ch;
        chan<request<std::monostate, std::vector<std::string>>>  list_ch;
        spawn_registry(std::move(join_ch.r), std::move(list_ch.r));

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

            // Pass restart-safe writer copies (the supervised imp may re-run).
            spawn(supervised([client_fd,
                              jw = join_ch.w.copy(),
                              lw = list_ch.w.copy()] {
                handle_client(client_fd, jw.copy(), lw.copy());
            }));
        }

        io::close(listen_fd);
        fprintf(stderr, "Server stopped.\n");
    });

    schedule();
}
