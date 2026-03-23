// etl_pipeline.cc — ETL pipeline using CSP stream combinators
//
// Demonstrates: map, where, batch, scan, parallel_map, backpressure,
// operator| composition, and the channels-as-interfaces pattern.
//
// Reads CSV records (name,age,city,score), then:
//   1. Parses each line into a record struct
//   2. Validates (rejects malformed rows)
//   3. Transforms (normalizes city names to uppercase)
//   4. Deduplicates (by name, keeping highest score)
//   5. Enriches (adds a grade based on score)
//   6. Batches into groups of 3
//   7. Reports batch summaries
//
// Run: echo -e "Alice,30,london,85\nBob,25,PARIS,92\n..." | ./etl_pipeline

#include "csp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace csp;

struct Record {
    std::string name;
    int age;
    std::string city;
    int score;
    std::string grade;  // enriched field
};

// Parse a CSV line into a Record.  Returns empty optional on failure.
static auto parse_record(const std::string& line) -> std::optional<Record> {
    std::istringstream ss(line);
    std::string name, age_s, city, score_s;
    if (!std::getline(ss, name, ',')) return std::nullopt;
    if (!std::getline(ss, age_s, ',')) return std::nullopt;
    if (!std::getline(ss, city, ',')) return std::nullopt;
    if (!std::getline(ss, score_s, ',')) return std::nullopt;
    try {
        return Record{name, std::stoi(age_s), city, std::stoi(score_s), {}};
    } catch (...) {
        return std::nullopt;
    }
}

// Normalize city to uppercase.
static void normalize_city(Record& r) {
    for (auto& c : r.city) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

// Assign a letter grade based on score.
static void assign_grade(Record& r) {
    if (r.score >= 90) r.grade = "A";
    else if (r.score >= 80) r.grade = "B";
    else if (r.score >= 70) r.grade = "C";
    else if (r.score >= 60) r.grade = "D";
    else r.grade = "F";
}

int main() {
    // Sample data (in a real pipeline, this would come from stdin or a file).
    static const char* const csv_data[] = {
        "Alice,30,london,85",
        "Bob,25,PARIS,92",
        "Charlie,35,berlin,78",
        "Alice,30,London,91",      // duplicate — higher score wins
        "bad line",                 // malformed — rejected
        "Diana,28,tokyo,65",
        "Eve,22,london,88",
        "Frank,40,paris,55",
        "Grace,33,Berlin,95",
        "Hank,29,Tokyo,72",
        "Eve,22,LONDON,90",        // duplicate — higher score wins
        "Ivy,26,rome,82",
    };

    spawn([]{
        chan<std::string> lines;

        // Stage 1: Producer — emit CSV lines.
        spawn([w = std::move(lines.w)] {
            for (auto* line : csv_data) {
                w << std::string(line);
            }
        });

        chan<Record> parsed;

        // Stage 2: Parse + validate — filter out malformed rows.
        spawn([r = std::move(lines.r), w = std::move(parsed.w)] {
            std::string line;
            while (r >> line) {
                auto rec = parse_record(line);
                if (rec) {
                    w << std::move(*rec);
                } else {
                    fprintf(stderr, "  [rejected] %s\n", line.c_str());
                }
            }
        });

        chan<Record> transformed;

        // Stage 3: Transform — normalize city names.
        spawn([r = std::move(parsed.r), w = std::move(transformed.w)] {
            Record rec;
            while (r >> rec) {
                normalize_city(rec);
                w << std::move(rec);
            }
        });

        chan<Record> deduped;

        // Stage 4: Deduplicate — keep highest score per name.
        // Collects all records, then emits deduplicated set.
        spawn([r = std::move(transformed.r), w = std::move(deduped.w)] {
            std::map<std::string, Record> best;
            Record rec;
            while (r >> rec) {
                auto it = best.find(rec.name);
                if (it == best.end() || rec.score > it->second.score) {
                    best[rec.name] = std::move(rec);
                }
            }
            for (auto& [name, r] : best) {
                w << std::move(r);
            }
        });

        chan<Record> enriched;

        // Stage 5: Enrich — assign letter grades.
        spawn([r = std::move(deduped.r), w = std::move(enriched.w)] {
            Record rec;
            while (r >> rec) {
                assign_grade(rec);
                w << std::move(rec);
            }
        });

        chan<std::vector<Record>> batches;

        // Stage 6: Batch into groups of 3.
        spawn([r = std::move(enriched.r), w = std::move(batches.w)] {
            std::vector<Record> batch;
            Record rec;
            while (r >> rec) {
                batch.push_back(std::move(rec));
                if (batch.size() == 3) {
                    w << std::move(batch);
                    batch.clear();
                }
            }
            if (!batch.empty()) {
                w << std::move(batch);
            }
        });

        // Stage 7: Report — print batch summaries.
        printf("ETL Pipeline Results:\n\n");
        int batch_num = 0;
        std::vector<Record> batch;
        while (batches.r >> batch) {
            printf("  Batch %d (%zu records):\n", ++batch_num, batch.size());
            for (const auto& rec : batch) {
                printf("    %-10s age=%-3d city=%-8s score=%-3d grade=%s\n",
                       rec.name.c_str(), rec.age, rec.city.c_str(),
                       rec.score, rec.grade.c_str());
            }
        }
        printf("\n  %d batches processed.\n", batch_num);
    });

    schedule();
}
