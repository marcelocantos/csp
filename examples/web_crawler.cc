// web_crawler.cc — Breadth-first web crawler with bounded concurrency
//
// Demonstrates: worker pool pattern, per-host rate limiting, deduplication,
// graceful shutdown via BLO (bidirectional lifecycle observability), and
// alt-based idle detection.
//
// Architecture:
//   - Simulated web graph: ~20 pages with cycles and dead ends
//   - Frontier channel: buffered chan<FetchReq> fed by coordinator
//   - Worker pool: N workers simulate fetching, enforce per-host rate limits
//   - Result channel: workers deliver CrawlResult back to coordinator
//   - Coordinator: deduplicates URLs, dispatches new discoveries, shuts down
//     when in_flight == 0 and no new URLs remain (by closing frontier writer)
//
// Shutdown cascade (BLO):
//   coordinator drops frontier writer
//     → workers see input death → drain → exit
//       → result channel closes (all writer copies dropped)
//         → coordinator result-reader loop exits → schedule() returns

#include "csp.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace csp;

// --- Simulated web graph ---

static const std::unordered_map<std::string, std::vector<std::string>> kWebGraph = {
    {"http://example.com/",          {"http://example.com/about",
                                      "http://example.com/blog",
                                      "http://news.example.org/latest"}},
    {"http://example.com/about",     {"http://example.com/",
                                      "http://example.com/contact"}},
    {"http://example.com/blog",      {"http://example.com/blog/post-1",
                                      "http://example.com/blog/post-2",
                                      "http://example.com/blog/post-3"}},
    {"http://example.com/contact",   {}},   // dead end
    {"http://example.com/blog/post-1", {"http://example.com/blog",
                                        "http://example.com/blog/post-2",
                                        "http://shop.example.net/widget"}},
    {"http://example.com/blog/post-2", {"http://example.com/blog",
                                        "http://news.example.org/latest"}},
    {"http://example.com/blog/post-3", {"http://example.com/blog",
                                        "http://example.com/blog/post-1"}},  // cycle
    {"http://news.example.org/latest", {"http://news.example.org/archive",
                                        "http://news.example.org/about",
                                        "http://example.com/"}},
    {"http://news.example.org/archive", {"http://news.example.org/latest",
                                         "http://news.example.org/page-2"}},
    {"http://news.example.org/about",  {"http://news.example.org/"}},
    {"http://news.example.org/",       {"http://news.example.org/latest",
                                        "http://news.example.org/archive"}},
    {"http://news.example.org/page-2", {}},  // dead end
    {"http://shop.example.net/widget", {"http://shop.example.net/",
                                        "http://shop.example.net/cart"}},
    {"http://shop.example.net/",       {"http://shop.example.net/widget",
                                        "http://shop.example.net/checkout"}},
    {"http://shop.example.net/cart",   {"http://shop.example.net/",
                                        "http://shop.example.net/checkout"}},
    {"http://shop.example.net/checkout", {}},  // dead end
};

// Extract hostname (between "://" and next "/").
static std::string hostname(const std::string& url) {
    auto start = url.find("://");
    if (start == std::string::npos) return url;
    start += 3;
    auto end = url.find('/', start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// --- Channel types ---

struct FetchReq {
    std::string url;
    int depth;
};

struct CrawlResult {
    std::string url;
    int depth;
    std::vector<std::string> links;  // outgoing URLs discovered
};

// --- Worker ---

static void run_worker(
    int id,
    reader<FetchReq> in,
    writer<CrawlResult> out)
{
    // Per-host last-fetch timestamps for rate limiting.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_fetch;
    constexpr auto kRateLimit = std::chrono::milliseconds(100);

    FetchReq req;
    while (in >> req) {
        // Per-host rate limiting: sleep until the minimum gap has elapsed.
        auto host = hostname(req.url);
        auto now = std::chrono::steady_clock::now();
        auto it = last_fetch.find(host);
        if (it != last_fetch.end()) {
            auto elapsed = now - it->second;
            if (elapsed < kRateLimit) {
                sleep(kRateLimit - elapsed);
            }
        }

        // Simulate fetch: look up the page in the web graph.
        std::vector<std::string> links;
        auto page = kWebGraph.find(req.url);
        if (page != kWebGraph.end()) {
            links = page->second;
        }

        // Simulate variable fetch latency (50–200ms based on URL length).
        auto fetch_ms = 50 + static_cast<int>(req.url.size() % 4) * 50;
        sleep(std::chrono::milliseconds(fetch_ms));

        last_fetch[host] = std::chrono::steady_clock::now();

        printf("  [worker %d] fetched %-45s depth=%d  links=%d\n",
               id, req.url.c_str(), req.depth, static_cast<int>(links.size()));

        out << CrawlResult{req.url, req.depth, std::move(links)};
    }
    // in died (frontier closed) → drop out writer copy → result ch closes
}

int main() {
    spawn([] {
        constexpr int kWorkers = 3;
        constexpr int kMaxDepth = 4;
        const std::string kSeed = "http://example.com/";

        chan<FetchReq>    frontier(32);   // buffered: coordinator can run ahead
        chan<CrawlResult> results;

        // Launch worker pool.
        for (int i = 0; i < kWorkers; ++i) {
            spawn([i,
                   in  = frontier.r.copy(),
                   out = results.w.copy()] () mutable {
                run_worker(i, std::move(in), std::move(out));
            });
        }
        // Release shared endpoint copies — workers hold their own.
        frontier.r = {};
        results.w  = {};

        printf("Web Crawler (%d workers, max depth %d):\n\n", kWorkers, kMaxDepth);

        // Coordinator: seed, dedup, dispatch, collect, shutdown.
        std::unordered_set<std::string> seen;
        int in_flight = 0;
        int total_crawled = 0;
        int total_skipped = 0;

        // Enqueue a URL if not yet seen and within depth limit.
        auto enqueue = [&](const std::string& url, int depth) {
            if (depth > kMaxDepth) return;
            if (!seen.insert(url).second) {
                ++total_skipped;
                return;
            }
            frontier.w << FetchReq{url, depth};
            ++in_flight;
        };

        enqueue(kSeed, 0);

        // Drain results; when idle (in_flight==0) close frontier → workers exit.
        CrawlResult res;
        while (results.r >> res) {
            ++total_crawled;
            --in_flight;

            for (const auto& link : res.links) {
                enqueue(link, res.depth + 1);
            }

            if (in_flight == 0) {
                // No more URLs in flight and nothing new was enqueued.
                // Close the frontier: workers will drain and exit, which
                // drops all result writer copies → results channel closes
                // → this loop exits.
                frontier.w = {};
            }
        }

        printf("\nCrawl complete: %d pages fetched, %d duplicates skipped.\n",
               total_crawled, total_skipped);
    });

    schedule();
}
