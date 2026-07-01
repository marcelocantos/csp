// log_aggregator.cc — Multi-source log aggregator with severity routing and alerting
//
// Demonstrates: merge (multi-reader fan-in), tick-driven time windows,
// routing imps (fan-out by severity), threshold alerting, and graceful
// cascade shutdown when all sources finish.
//
// Architecture:
//   - Source imps: simulate log tailing from web, api, db, and auth services,
//     emitting LogEntry structs at varying rates. A burst of errors is
//     injected into the auth service midway to trigger the alert threshold.
//   - Merge: csp::part::merge combines all four sources into one stream.
//   - Routing: a map passthrough prints each entry, then csp::part::partition
//     fans out to info/warn/error streams by severity (DBG is dropped).
//   - Alert monitor imp: counts errors per 1-second window; prints ALERT
//     when the count reaches the threshold (3).
//   - Aggregator imp: counts info/warn events per service per window,
//     prints a summary on each 1-second tick.

#include "csp.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

// --- Data types ---

enum class Sev { DBG, INFO, WARN, ERR };

static const char* sev_name(Sev s) {
    switch (s) {
        case Sev::DBG:  return "DEBUG";
        case Sev::INFO: return "INFO ";
        case Sev::WARN: return "WARN ";
        case Sev::ERR:  return "ERROR";
    }
    return "?";
}

struct LogEntry {
    std::string service;
    Sev         severity;
    std::string message;
};

// --- Source helper ---
// Runs inside a spawned imp: emits pre-canned entries with a fixed delay.

static void emit_logs(
    writer<LogEntry>&&                                 w,
    const char*                                        svc,
    std::vector<std::pair<Sev, const char*>>           entries,
    std::chrono::milliseconds                          delay)
{
    for (auto& [sev, msg] : entries) {
        sleep(delay);
        if (!(w << LogEntry{svc, sev, msg})) return;
    }
}

// --- Main ---

int main() {
    spawn([] {
        printf("Log Aggregator — 4 services, 1-second windows, alert at >=3 errors\n\n");

        chan<LogEntry> web_ch, api_ch, db_ch, auth_ch;

        spawn([w = std::move(web_ch.w)] () mutable {
            emit_logs(std::move(w), "web ", {
                {Sev::INFO, "GET /index.html 200"},
                {Sev::INFO, "GET /about.html 200"},
                {Sev::WARN, "GET /missing.html 404"},
                {Sev::INFO, "POST /api/login 200"},
                {Sev::INFO, "GET /dashboard 200"},
                {Sev::WARN, "slow response: 1.2s"},
                {Sev::INFO, "GET /api/data 200"},
                {Sev::INFO, "GET /static/app.js 200"},
            }, 120ms);
        });

        spawn([w = std::move(api_ch.w)] () mutable {
            emit_logs(std::move(w), "api ", {
                {Sev::DBG,  "request received"},
                {Sev::INFO, "auth token validated"},
                {Sev::INFO, "query executed in 12ms"},
                {Sev::WARN, "rate limit approaching: 85%"},
                {Sev::INFO, "response sent 200"},
                {Sev::INFO, "cache hit ratio: 0.92"},
                {Sev::WARN, "downstream latency spike"},
                {Sev::INFO, "graceful drain started"},
            }, 90ms);
        });

        spawn([w = std::move(db_ch.w)] () mutable {
            emit_logs(std::move(w), "db  ", {
                {Sev::INFO, "connection pool: 4/20"},
                {Sev::DBG,  "query plan selected"},
                {Sev::INFO, "index scan: users (3 rows)"},
                {Sev::WARN, "lock wait: 230ms"},
                {Sev::INFO, "transaction committed"},
                {Sev::INFO, "vacuum completed"},
                {Sev::INFO, "checkpoint written"},
            }, 150ms);
        });

        // auth: normal start, then a burst of errors to trigger the alert.
        spawn([w = std::move(auth_ch.w)] () mutable {
            emit_logs(std::move(w), "auth", {
                {Sev::INFO, "session created: user=42"},
                {Sev::INFO, "token refreshed"},
                {Sev::ERR,  "invalid signature — rejecting"},
                {Sev::ERR,  "replay attack detected"},
                {Sev::ERR,  "token expired: forced logout"},
                {Sev::ERR,  "brute-force threshold exceeded"},
                {Sev::WARN, "rate-limiting client 10.0.0.5"},
                {Sev::INFO, "lockout cleared"},
                {Sev::INFO, "session closed normally"},
            }, 100ms);
        });

        // Merge all four sources into one stream.
        std::vector<reader<LogEntry>> srcs;
        srcs.push_back(std::move(web_ch.r));
        srcs.push_back(std::move(api_ch.r));
        srcs.push_back(std::move(db_ch.r));
        srcs.push_back(std::move(auth_ch.r));
        auto merged = merge<LogEntry>(std::move(srcs)).spawn();

        // Print every entry, then route by severity with the partition
        // combinator: INFO/WARN/ERR go to outputs 0/1/2; DBG maps out of
        // range and is dropped. partition itself does no printing, so a
        // map passthrough handles the per-entry log line first.
        auto printed = map<LogEntry>([](LogEntry e) {
            printf("  [%s] %-4s %s\n",
                   sev_name(e.severity), e.service.c_str(), e.message.c_str());
            return e;
        }).spawn(std::move(merged));

        auto routed = partition<LogEntry>(std::move(printed), 3,
            [](const LogEntry& e) -> size_t {
                switch (e.severity) {
                    case Sev::INFO: return 0;
                    case Sev::WARN: return 1;
                    case Sev::ERR:  return 2;
                    default:        return 3;   // DBG (out of range) → dropped
                }
            });
        auto info_r  = std::move(routed[0]);
        auto warn_r  = std::move(routed[1]);
        auto error_r = std::move(routed[2]);

        // Alert monitor: count errors per 1-second window.
        constexpr int kAlertThreshold = 3;
        spawn([er = std::move(error_r)] () mutable {
            auto ticks = tick(1s);
            time_point tp;
            LogEntry e;
            int window_errors = 0;
            bool alerted = false;
            for (;;) {
                switch (alt(er >> e, ticks >> tp)) {
                case 0:   // error arrived
                    if (++window_errors >= kAlertThreshold && !alerted) {
                        printf("\n  *** ALERT: %d errors in current window"
                               " (threshold=%d) ***\n\n",
                               window_errors, kAlertThreshold);
                        alerted = true;
                    }
                    break;
                case 1:   // tick: reset window
                    window_errors = 0;
                    alerted = false;
                    break;
                case ~0:  // er closed — all sources done
                    return;
                }
            }
        });

        // Aggregator: count info/warn per service, flush summary on each tick.
        struct Counts { int info = 0, warn = 0; };
        spawn([ir = std::move(info_r), wr = std::move(warn_r)] () mutable {
            auto ticks = tick(1s);
            time_point tp;
            LogEntry e;
            std::map<std::string, Counts> window;
            int window_num = 0;

            auto flush = [&] {
                if (window.empty()) return;
                printf("\n  -- Window %d summary --\n", ++window_num);
                for (auto& [svc, c] : window) {
                    printf("    %-4s  info=%-2d warn=%-2d\n",
                           svc.c_str(), c.info, c.warn);
                }
                printf("\n");
                window.clear();
            };

            for (;;) {
                switch (alt(ir >> e, wr >> e, ticks >> tp)) {
                case 0: window[e.service].info++; break;
                case 1: window[e.service].warn++; break;
                case 2: flush(); break;
                default: flush(); return;  // any channel death → flush and exit
                }
            }
        });
    });

    schedule();

    printf("  Aggregator done.\n");
}
