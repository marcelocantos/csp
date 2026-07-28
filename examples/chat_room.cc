// chat_room.cc — Dynamic pub/sub
//
// A chat server where users dynamically join and leave. The server
// uses fanout to broadcast each message to all connected users.
// New users register by sending their writer endpoint via a control
// channel (channel of channels — a first-class CSP pattern).
// When a user's channel dies, fanout automatically detects it and
// removes them.
//
// This kind of dynamic topology is natural in CSP but requires
// careful mutex + subscriber-list management in traditional code.

#include "csp.h"

#include <cstdio>
#include <string>

using namespace csp;
using namespace csp::part;

int main() {
    spawn([]{
        printf("Chat room demo:\n");

        // new_sub: subscriber registration channel.
        // Write a writer<string> here to register a new subscriber.
        writer<writer<std::string>> new_sub;

        // Spawn fanout: it reads new subscriber endpoints from new_sub,
        // and returns a reader from which we get input channels.
        auto new_in = fanout<std::string>.spawn(--new_sub);

        // Helper to add a user
        auto add_user = [&](const char* name) {
            auto [w, r] = chan<std::string>{};
            new_sub << std::move(w);  // register this user's writer with fanout
            spawn([name, r = std::move(r)]{
                for (std::string msg; r >> msg;) {
                    printf("  [%s] %s\n", name, msg.c_str());
                }
                printf("  [%s] left the chat\n", name);
            });
        };

        // Users join
        add_user("Alice");
        add_user("Bob");

        // Get an input channel from fanout
        writer<std::string> in;
        new_in >> in;
        new_in = {};

        // Broadcast messages
        in << std::string("Hello everyone!");
        csp::yield();  // let receivers print

        // Charlie joins mid-conversation
        add_user("Charlie");

        in << std::string("Welcome Charlie!");
        csp::yield();

        in << std::string("Goodbye!");
        csp::yield();

        // Shut down: close input, then subscribers
        in = {};
        new_sub = {};
    });

    await_completion();
}
