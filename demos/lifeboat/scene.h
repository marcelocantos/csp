// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <algorithm>
#include <cmath>
#include <compare>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace lifeboat {
struct Point {
    double x = 0, y = 0, z = 0;
    auto operator<=>(Point const&) const = default;
};
struct Keyframe { double t; Point p; };
struct Motion {
    std::string key, kind, phase;
    int cargo = 0, actor = 0;
    unsigned long revision = 0;
    double at = 0, duration = 0;
    std::vector<Keyframe> frames;
};
inline Point interpolate(Motion const& motion, double now) {
    double t = motion.duration > 0 ? std::clamp((now - motion.at) / motion.duration, 0.0, 1.0) : 1;
    if (motion.frames.empty()) return {};
    for (size_t i = 1; i < motion.frames.size(); ++i) if (t <= motion.frames[i].t) {
        auto const& a = motion.frames[i - 1];
        auto const& b = motion.frames[i];
        double u = (t - a.t) / (b.t - a.t);
        // Each server-authored segment eases at its named waypoints.
        u = u * u * (3 - 2 * u);
        return {a.p.x + (b.p.x - a.p.x) * u, a.p.y + (b.p.y - a.p.y) * u, a.p.z + (b.p.z - a.p.z) * u};
    }
    return motion.frames.back().p;
}
// Motion fields are closed, server-authored identifiers, never request text.
inline std::string motion_json(Motion const& m) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4)
        << "{\"key\":\"" << m.key << "\",\"kind\":\"" << m.kind << "\",\"phase\":\"" << m.phase
        << "\",\"cargo\":" << m.cargo << ",\"actor\":" << m.actor << ",\"revision\":" << m.revision
        << ",\"at\":" << m.at << ",\"duration\":" << m.duration << ",\"frames\":[";
    bool comma = false;
    for (auto const& f : m.frames) {
        if (comma) out << ',';
        comma = true;
        out << '[' << f.t << ',' << f.p.x << ',' << f.p.y << ',' << f.p.z << ']';
    }
    return out.str() + "]}";
}
} // namespace lifeboat
