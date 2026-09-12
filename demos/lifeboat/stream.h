// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
#pragma once

// Included after the coordinator's Query, Reply, ask(), and JSON serializers.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

namespace lifeboat {

// Upgrade with this receive limit: only a decimal sequence ACK is accepted.
inline constexpr size_t stream_ack_limit = 20;

inline void stream_scene(csp::ws::conn socket, writer<Query> requests) {
    constexpr auto sample_period = std::chrono::milliseconds(50);
    constexpr auto telemetry_period = std::chrono::milliseconds(200);
    constexpr auto idle_heartbeat = std::chrono::seconds(1);
    constexpr auto ack_timeout = std::chrono::seconds(5);
    // JavaScript must be able to echo seq without rounding it.
    constexpr uint64_t largest_sequence = 9007199254740991ULL;
    struct Close {
        csp::ws::conn& socket;
        ~Close() { socket.close(); }
    } close{socket};

    std::map<std::string, unsigned long> revisions;
    std::string last_telemetry;
    int run = 0;
    uint64_t sequence = 0;
    auto next_sample = csp::now();
    auto last_frame = next_sample;
    auto last_state = next_sample;

    for (;;) {
        csp::ws::message inbound;
        // No client messages are allowed outside the one outstanding ACK.
        if (csp::prialt(socket.recv >> inbound,
                        csp::after(next_sample - csp::now()) >> nullptr) != 1) return;
        auto reply = ask(requests, Command::snapshot);
        auto const& state = reply.state;
        auto sampled = csp::now();
        next_sample = sampled + sample_period;
        bool reset = sequence == 0 || run != state.run;
        bool moving = false;
        std::ostringstream motions, removed;
        bool changed = false, comma = false;
        for (auto const& [key, motion] : state.motions) {
            moving |= motion.at + motion.duration > state.now;
            auto old = revisions.find(key);
            if (!reset && old != revisions.end() && old->second == motion.revision) continue;
            if (comma) motions << ',';
            comma = changed = true;
            motions << motion_json(motion);
        }
        comma = false;
        if (!reset) {
            for (auto const& [key, revision] : revisions) {
                if (state.motions.contains(key)) continue;
                if (comma) removed << ',';
                comma = changed = true;
                removed << "{\"key\":" << quote(key) << ",\"at\":"
                        << std::setprecision(12) << state.now << '}';
            }
        }

        // The clock is carried by every frame; it alone is not a telemetry
        // change. Compare the existing serializer with only its clock zeroed.
        double now = reply.state.now;
        reply.state.now = 0;
        auto telemetry_key = json(reply);
        reply.state.now = now;
        bool include_state = reset || (sampled - last_state >= telemetry_period &&
                                       telemetry_key != last_telemetry);
        auto heartbeat = moving ? csp::duration(telemetry_period) : csp::duration(idle_heartbeat);
        if (!reset && !changed && !include_state && sampled - last_frame < heartbeat) continue;
        if (sequence == largest_sequence) return;

        std::ostringstream frame;
        frame << std::setprecision(12) << "{\"type\":\"scene\",\"seq\":" << ++sequence
              << ",\"now\":" << state.now << ",\"run\":" << state.run
              << ",\"reset\":" << (reset ? "true" : "false")
              << ",\"motions\":[" << motions.str() << "],\"removed\":[" << removed.str() << ']';
        if (include_state) frame << ",\"state\":" << json(reply);
        frame << '}';
        auto payload = frame.str();
        csp::ws::message outbound{csp::ws::opcode::text, csp::bytes(payload.begin(), payload.end())};
        auto deadline = sampled + ack_timeout;
        // Channel admission and ACK share one deadline. A socket whose writer
        // is already stuck cannot postpone expiry by refusing the next frame.
        if (csp::prialt(socket.recv >> inbound, socket.send << std::move(outbound),
                        csp::after(deadline - csp::now()) >> nullptr) != 1) return;
        if (csp::prialt(socket.recv >> inbound,
                        csp::after(deadline - csp::now()) >> nullptr) != 0) return;
        auto expected = std::to_string(sequence);
        if (inbound.op != csp::ws::opcode::text || inbound.data.size() != expected.size() ||
            !std::equal(inbound.data.begin(), inbound.data.end(), expected.begin())) return;

        // These describe only the acknowledged view. While waiting above,
        // there are no snapshot requests or queued outgoing scene updates.
        revisions.clear();
        for (auto const& [key, motion] : state.motions) revisions.emplace(key, motion.revision);
        run = state.run;
        last_frame = sampled;
        if (include_state) { last_state = sampled; last_telemetry = std::move(telemetry_key); }
    }
}

} // namespace lifeboat
