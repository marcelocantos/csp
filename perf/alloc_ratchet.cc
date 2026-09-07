// Allocation ratchet for the CSP runtime's core operations.
//
// Wall-clock benchmarks (bench/, nanobench) cannot gate: on a loaded
// machine the same binary varies by tens of percent, and nanobench itself
// flags most of the channel benchmarks unstable. Heap allocation counts
// for a fixed workload do not vary with load at all, so they are what the
// gate locks.
//
// The numbers below are per fixed workload, not per operation, so a
// change that moves an allocation out of a loop shows up as a large,
// obvious delta rather than a rounding difference.
//
// Run `make perf` to check against docs/perf/baseline.md, or
// `./build/normal/csp_ratchet --write` to re-record it in the commit that
// changes the measured code.

#include <csp/csp.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Counters live outside any C++ object so that they cannot themselves
// allocate. They are plain globals for the same reason.
std::size_t g_allocs = 0;
std::size_t g_bytes = 0;
bool g_counting = false;

struct Counts {
    std::size_t allocs;
    std::size_t bytes;
};

Counts snapshot() { return {g_allocs, g_bytes}; }

Counts since(Counts before) {
    return {g_allocs - before.allocs, g_bytes - before.bytes};
}

}  // namespace

void* operator new(std::size_t n) {
    if (g_counting) {
        g_allocs++;
        g_bytes += n;
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

// Workload sizes. Small enough that the whole ratchet runs in well under
// a second, large enough that a per-operation allocation is unmissable.
constexpr int OPS = 2'000;
constexpr int ALT_CHANNELS = 8;
constexpr int SPAWN_COUNT = 500;

/// One locked measurement.
struct Row {
    std::string name;
    std::size_t allocs;
    std::size_t bytes;
};

/// Unbuffered send/receive between two imps: the core channel rendezvous.
void workload_send_recv() {
    csp::chan<int> ch;
    csp::spawn([w = ch.w.copy()] {
        for (int i = 0; i < OPS; i++) w << i;
    });
    int sum = 0;
    csp::spawn([r = ch.r.copy(), &sum] {
        int n;
        for (int i = 0; i < OPS; i++) {
            r >> n;
            sum += n;
        }
    });
    ch.release();
    csp::await_completion();
}

/// Prioritised alt across several channels: the selection path. Each end
/// is copied into the imp that uses it, as the channel benchmark does —
/// an imp that borrows a reader the outer scope then releases crashes.
void workload_prialt() {
    csp::chan<int> chans[ALT_CHANNELS];
    for (int k = 0; k < ALT_CHANNELS; k++) {
        csp::spawn([w = chans[k].w.copy()] {
            for (int i = 0; i < OPS / ALT_CHANNELS; i++) w << i;
        });
    }
    int sum = 0;
    csp::spawn([&sum, r0 = chans[0].r.copy(), r1 = chans[1].r.copy(), r2 = chans[2].r.copy(),
                r3 = chans[3].r.copy(), r4 = chans[4].r.copy(), r5 = chans[5].r.copy(),
                r6 = chans[6].r.copy(), r7 = chans[7].r.copy()] {
        int n;
        for (int i = 0; i < OPS; i++) {
            csp::prialt(r0 >> n, r1 >> n, r2 >> n, r3 >> n, r4 >> n, r5 >> n, r6 >> n, r7 >> n);
            sum += n;
        }
    });
    for (auto& c : chans) c.release();
    csp::await_completion();
}

/// Spawn and completion alone, with no channel traffic: what a process
/// costs to create and retire.
void workload_spawn() {
    int sum = 0;
    for (int i = 0; i < SPAWN_COUNT; i++) csp::spawn([&sum, i] { sum += i; });
    csp::await_completion();
}

/// Measured runs per workload. The channel workloads are allocation-free
/// in steady state, but `prialt` occasionally allocates an extra waiting
/// record depending on which writer the scheduler runs first, so the
/// minimum over a few runs is taken: it is the count when no scheduling
/// accident intervenes, and it is stable enough to lock at 1%.
constexpr int REPEATS = 5;

Row measure(const char* name, void (*workload)()) {
    // One unmeasured run so that first-touch lazies (stack pool growth,
    // one-off runtime tables) are not attributed to a measured run.
    workload();
    Counts best{static_cast<std::size_t>(-1), static_cast<std::size_t>(-1)};
    for (int i = 0; i < REPEATS; i++) {
        g_counting = true;
        Counts before = snapshot();
        workload();
        Counts delta = since(before);
        g_counting = false;
        if (delta.allocs < best.allocs) best.allocs = delta.allocs;
        if (delta.bytes < best.bytes) best.bytes = delta.bytes;
    }
    return Row{name, best.allocs, best.bytes};
}

const char* const BASELINE_PATH = "docs/perf/baseline.md";
const char* const BEGIN_MARKER = "<!-- perf-baseline:begin -->";
const char* const END_MARKER = "<!-- perf-baseline:end -->";
// Allocation counts for a fixed workload are deterministic, so the band
// only has to absorb container growth that depends on address or hash
// ordering. Anything past it, in either direction, is a deliberate change.
constexpr double TOLERANCE_PCT = 1.0;

std::string render(const std::vector<Row>& rows) {
    std::string s = "| workload | allocs | bytes |\n|---|---:|---:|\n";
    for (const auto& r : rows) {
        s += "| " + r.name + " | " + std::to_string(r.allocs) + " | " + std::to_string(r.bytes) +
             " |\n";
    }
    return s;
}

std::string read_file(const char* path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<Row> parse(const std::string& md, bool* ok) {
    std::vector<Row> rows;
    auto begin = md.find(BEGIN_MARKER);
    auto end = md.find(END_MARKER);
    if (begin == std::string::npos || end == std::string::npos) {
        *ok = false;
        return rows;
    }
    *ok = true;
    std::string body = md.substr(begin, end - begin);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string line = body.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty() || line[0] != '|') continue;
        std::vector<std::string> cells;
        size_t c = 1;
        while (c <= line.size()) {
            size_t bar = line.find('|', c);
            if (bar == std::string::npos) break;
            std::string cell = line.substr(c, bar - c);
            size_t a = cell.find_first_not_of(" \t");
            size_t b = cell.find_last_not_of(" \t");
            cells.push_back(a == std::string::npos ? "" : cell.substr(a, b - a + 1));
            c = bar + 1;
        }
        if (cells.size() != 3) continue;
        if (cells[0] == "workload" || cells[0].rfind("---", 0) == 0) continue;
        rows.push_back(Row{cells[0], std::strtoull(cells[1].c_str(), nullptr, 10),
                           std::strtoull(cells[2].c_str(), nullptr, 10)});
    }
    return rows;
}

bool within_band(std::size_t now, std::size_t base) {
    if (base == 0) return now == 0;
    double delta = 100.0 * (static_cast<double>(now) - static_cast<double>(base)) /
                   static_cast<double>(base);
    return delta < 0 ? -delta <= TOLERANCE_PCT : delta <= TOLERANCE_PCT;
}

int check(const std::vector<Row>& rows) {
    bool ok = false;
    std::vector<Row> base = parse(read_file(BASELINE_PATH), &ok);
    if (!ok) {
        std::fprintf(stderr,
                     "%s: no %s block — record one with ./build/normal/csp_ratchet --write\n",
                     BASELINE_PATH, BEGIN_MARKER);
        return 1;
    }
    int failures = 0;
    for (const auto& now : rows) {
        const Row* want = nullptr;
        for (const auto& b : base)
            if (b.name == now.name) want = &b;
        if (!want) {
            std::fprintf(stderr, "  %s: not in baseline (lock it with --write)\n",
                         now.name.c_str());
            failures++;
            continue;
        }
        for (auto field : {0, 1}) {
            std::size_t n = field == 0 ? now.allocs : now.bytes;
            std::size_t w = field == 0 ? want->allocs : want->bytes;
            if (within_band(n, w)) continue;
            const char* label = field == 0 ? "allocs" : "bytes";
            const char* verdict = n < w ? "leaner — lock it in with --write" : "regression";
            std::fprintf(stderr, "  %s %s: %zu -> %zu (%s)\n", now.name.c_str(), label, w, n,
                         verdict);
            failures++;
        }
    }
    for (const auto& b : base) {
        bool present = false;
        for (const auto& r : rows)
            if (r.name == b.name) present = true;
        if (!present) {
            std::fprintf(stderr, "  %s: in baseline but no longer measured\n", b.name.c_str());
            failures++;
        }
    }
    if (failures) {
        std::fprintf(stderr, "alloc ratchet FAILED (%d)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "alloc ratchet OK (within +/-%.0f%% both ways)\n", TOLERANCE_PCT);
    return 0;
}

int write_baseline(const std::vector<Row>& rows) {
    std::string md = read_file(BASELINE_PATH);
    std::string table = render(rows);
    auto begin = md.find(BEGIN_MARKER);
    auto end = md.find(END_MARKER);
    std::string out;
    if (begin != std::string::npos && end != std::string::npos) {
        out = md.substr(0, begin) + BEGIN_MARKER + "\n" + table + md.substr(end);
    } else {
        out = md + "\n" + BEGIN_MARKER + "\n" + table + END_MARKER + "\n";
    }
    std::ofstream o(BASELINE_PATH);
    if (!o) {
        std::fprintf(stderr, "cannot write %s\n", BASELINE_PATH);
        return 1;
    }
    o << out;
    std::fprintf(stderr, "baseline written to %s\n", BASELINE_PATH);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool write = argc > 1 && std::strcmp(argv[1], "--write") == 0;
    std::vector<Row> rows{
        measure("send/recv", workload_send_recv),
        measure("prialt/8ch", workload_prialt),
        measure("spawn", workload_spawn),
    };
    std::printf("%s", render(rows).c_str());
    return write ? write_baseline(rows) : check(rows);
}
