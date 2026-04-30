// etl_pipeline.cc — ETL pipeline using CSP stream combinators
//
// Pipeline stages and which combinators they use:
//
//   chain        — concatenate two CSV batches sequentially (two input "files")
//   map          — parse CSV line → Record
//   where        — validate: drop malformed rows
//   map          — transform: normalize city to uppercase
//   make_filter  — dedup + city-rank annotation (first-wins per name)
//   map          — enrich: assign letter grade from score
//   scan         — running totals: fold over stream tracking count, score-sum, grade-buckets
//   batch        — collect 3 records per batch
//   parallel_map — format each batch (2 concurrent workers, ordered output)
//   for-range    — sink: print formatted output to stdout
//
// Backpressure: CSP channels are synchronous rendezvous by default — each
// stage blocks until the next is ready, providing natural backpressure
// throughout the pipeline. To decouple stages with a fixed-size buffer,
// use chan<T>(n) (buffered channel with ring buffer of capacity n).
//
// How to run:
//   ./etl_pipeline
//
// Expected output:
//   24 CSV rows ingested (2 batches of 12). After 2 rejects and 5 dedup
//   drops, 17 unique records flow through to 6 batches. Each line shows
//   name, age, normalized city, score, grade, city_rank, and running totals.
//
// Sink choice: stdout. Replace the print with sqlite3_exec INSERT and link
// with -lsqlite3 for a database sink.

#include "csp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace csp;
using namespace csp::part;

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

struct Record {
    std::string name;
    int         age       = 0;
    std::string city;
    int         score     = 0;
    char        grade     = '?'; // assigned in enrich stage
    int         city_rank = 0;   // assigned in dedup stage
};

// Running totals emitted by scan alongside each Record.
struct Snapshot {
    Record rec;
    int    count     = 0;
    int    score_sum = 0;
    int    grade_a   = 0;
    int    grade_b   = 0;
    int    grade_cf  = 0; // C, D, or F
};

// ---------------------------------------------------------------------------
// Fixed input data — two CSV "files" concatenated by chain (24 rows total)
// ---------------------------------------------------------------------------

static std::vector<std::string> const kBatchA = {
    "Alice,30,london,85",
    "Bob,25,PARIS,92",
    "Charlie,35,berlin,78",
    "Diana,28,tokyo,65",
    "Eve,22,london,88",
    "Frank,40,paris,55",
    "Grace,33,Berlin,95",
    "Hank,29,Tokyo,72",
    "Ivy,26,rome,82",
    "Jack,31,madrid,74",
    "bad line",               // malformed — rejected by validate stage
    "Kate,27,rome,91",
};

static std::vector<std::string> const kBatchB = {
    "Leo,24,london,69",
    "Alice,30,London,91",    // dup — first-wins; Alice(85) from Batch A kept
    "Mia,36,paris,83",
    "Bob,25,Paris,88",       // dup — first-wins; Bob(92) from Batch A kept
    "Nate,32,berlin,76",
    "Grace,33,berlin,97",    // dup — first-wins; Grace(95) from Batch A kept
    "Olivia,29,tokyo,84",
    "Eve,22,LONDON,90",      // dup — first-wins; Eve(88) from Batch A kept
    "Pablo,38,rome,61",
    "Kate,27,Rome,88",       // dup — first-wins; Kate(91) from Batch A kept
    "",                      // blank line — rejected by validate stage
    "Quinn,23,madrid,79",
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::optional<Record> parse_csv(std::string const& line) {
    std::istringstream ss(line);
    std::string name, age_s, city, score_s;
    if (!std::getline(ss, name,    ',')) return std::nullopt;
    if (!std::getline(ss, age_s,   ',')) return std::nullopt;
    if (!std::getline(ss, city,    ',')) return std::nullopt;
    if (!std::getline(ss, score_s, ',')) return std::nullopt;
    if (name.empty() || city.empty())    return std::nullopt;
    try {
        return Record{name, std::stoi(age_s), city, std::stoi(score_s), '?', 0};
    } catch (...) {
        return std::nullopt;
    }
}

static char grade_for(int score) {
    if (score >= 90) return 'A';
    if (score >= 80) return 'B';
    if (score >= 70) return 'C';
    if (score >= 60) return 'D';
    return 'F';
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    printf("ETL Pipeline — chain|parse|validate|transform|"
           "dedup|enrich|scan|batch|parallel_map|sink\n\n");

    spawn([] {
        // ------------------------------------------------------------------
        // Stage 1: chain — concatenate two input batches sequentially.
        // Each reader is exhausted in order before the next is started.
        // ------------------------------------------------------------------
        std::vector<reader<std::string>> sources;
        sources.push_back(enumerate(kBatchA).spawn());
        sources.push_back(enumerate(kBatchB).spawn());
        auto lines = chain<std::string>(std::move(sources)).spawn();

        // ------------------------------------------------------------------
        // Stage 2: map — parse each CSV line into an optional<Record>.
        // Stage 3: where — validate: keep only successfully parsed rows.
        // Stage 4: map — unwrap the optional (all have values here).
        // Composed via operator| into one filter: string → Record.
        // Synchronous channels between each stage provide natural backpressure.
        // ------------------------------------------------------------------
        auto records =
            (map<std::string, std::optional<Record>>(parse_csv)
             | where<std::optional<Record>>([](auto const& r) {
                   if (!r) fprintf(stderr, "  [rejected]\n");
                   return r.has_value();
               })
             | map<std::optional<Record>, Record>([](auto r) { return *r; })
            ).spawn(std::move(lines));

        // ------------------------------------------------------------------
        // Stage 5: map — transform: normalize city names to uppercase.
        // ------------------------------------------------------------------
        auto normalized = map<Record>([](Record r) {
            for (auto& c : r.city)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return r;
        }).spawn(std::move(records));

        // ------------------------------------------------------------------
        // Stage 6: make_filter — deduplicate + annotate city rank.
        // First-wins: keeps the first record seen for each name; drops
        // subsequent records with the same name. Also assigns city_rank
        // (count of unique records seen so far for that city).
        // ------------------------------------------------------------------
        auto deduped = make_filter<Record>(
            [](reader<Record> in, writer<Record> out) {
                std::set<std::string>      seen;
                std::map<std::string, int> city_count;
                for (Record r; alt(in >> r, ~out) == 0;) {
                    if (!seen.insert(r.name).second) {
                        fprintf(stderr, "  [dedup] dropped: %s\n", r.name.c_str());
                        continue;
                    }
                    r.city_rank = ++city_count[r.city];
                    if (!(out << r)) return;
                }
            }).spawn(std::move(normalized));

        // ------------------------------------------------------------------
        // Stage 7: map — enrich: assign letter grade from score.
        // ------------------------------------------------------------------
        auto enriched = map<Record>([](Record r) {
            r.grade = grade_for(r.score);
            return r;
        }).spawn(std::move(deduped));

        // ------------------------------------------------------------------
        // Stage 8: scan — running totals.
        // scan<In, S>(init, f): after each Record, emits updated accumulator.
        // Here S = Snapshot (record + running grade-bucket counts).
        // ------------------------------------------------------------------
        auto snapshots = scan<Record, Snapshot>(
            Snapshot{},
            [](Snapshot s, Record r) {
                s.count++;
                s.score_sum += r.score;
                if      (r.grade == 'A') s.grade_a++;
                else if (r.grade == 'B') s.grade_b++;
                else                     s.grade_cf++;
                s.rec = r;
                return s;
            }
        ).spawn(std::move(enriched));

        // ------------------------------------------------------------------
        // Stage 9: batch — collect 3 Snapshots per batch.
        // Partial last batch (< 3 items) is flushed when input closes.
        // ------------------------------------------------------------------
        auto batched = batch<Snapshot>(3).spawn(std::move(snapshots));

        // ------------------------------------------------------------------
        // Stage 10: parallel_map — format each batch on 2 workers.
        // ordered=true preserves input sequence despite concurrent workers.
        // ------------------------------------------------------------------
        using Batch = std::vector<Snapshot>;
        auto formatted = parallel_map<Batch, std::string>(
            2,
            [](Batch b) {
                std::string out;
                for (auto const& snap : b) {
                    auto const& r = snap.rec;
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "  %-10s age=%-3d city=%-8s score=%-3d grade=%c"
                        " rank=%d  [n=%d avg=%.0f A=%d B=%d CF=%d]\n",
                        r.name.c_str(), r.age, r.city.c_str(),
                        r.score, r.grade, r.city_rank,
                        snap.count,
                        snap.count ? (double)snap.score_sum / snap.count : 0.0,
                        snap.grade_a, snap.grade_b, snap.grade_cf);
                    out += buf;
                }
                return out;
            },
            {.ordered = true}
        ).spawn(std::move(batched));

        // ------------------------------------------------------------------
        // Stage 11: sink — print formatted batches to stdout.
        // For a SQLite sink: swap printf for sqlite3_exec INSERT rows
        // from the batch, and link with -lsqlite3.
        // ------------------------------------------------------------------
        int n = 0;
        for (std::string s; formatted >> s;) {
            printf("Batch %d:\n%s", ++n, s.c_str());
        }
        printf("\n%d batches written.\n", n);
    });

    schedule();
}
