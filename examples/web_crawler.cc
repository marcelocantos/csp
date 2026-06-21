// web_crawler.cc — Breadth-first web crawler with bounded concurrency
//
// Demonstrates: in-process HTTP server + client, worker pool, per-host rate
// limiting, URL deduplication, and graceful BLO shutdown.
//
// HTTP client: hand-rolled HTTP/1.0 GET over csp::net::dial.
//   csp::http::get always emits its own "Host" header from the URL (e.g.
//   "Host: 127.0.0.1:PORT"), preventing us from supplying the virtual
//   hostname needed for server-side routing.  We therefore use net::dial
//   directly and construct the raw request bytes ourselves.  This also
//   avoids chunked transfer and compression — pure HTTP/1.0 semantics.
//
// Architecture:
//   In-process server: csp::http::serve on port 0 (OS-assigned) serving a
//     linked-page graph of ~15 pages across 3 virtual hosts.
//     Pages are selected by the "Host" header; body is a newline-separated
//     list of absolute URLs that the page links to.
//
//   Frontier: buffered chan<FetchReq>(32) — coordinator enqueues, workers dequeue.
//
//   Worker pool: 4 workers, each with per-host rate-limit state (100ms minimum
//     gap per host). Workers call raw_get to retrieve a page, parse the link
//     list from the body, then send a CrawlResult back.
//
//   Coordinator: seeds the frontier, deduplicates via unordered_set, collects
//     results, re-enqueues new links. When in_flight reaches 0 with nothing
//     new to enqueue, drops frontier.w → workers exit → results closes →
//     coordinator exits → drops stop_ch.w → server-accept imp exits.
//
// Shutdown cascade (BLO):
//   coordinator drops frontier.w
//     → workers' input loop exits → workers drop their results.w copies
//       → results channel closes → coordinator's result loop exits
//         → coordinator drops stop_ch.w → ~stop_r fires in server imp
//           → server-accept imp returns → schedule() returns

#include "csp.h"
#include "csp/net.h"
#include "csp/http.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace csp;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// In-process web graph
// ---------------------------------------------------------------------------
//
// Three virtual hosts, linked to each other. "Host" header selects the host.
// Each page body is newline-separated absolute URLs (the links on that page).

static const std::unordered_map<std::string,
    std::unordered_map<std::string, std::vector<std::string>>> kGraph = {
    {"example.com", {
        {"/",            {"http://example.com/about",
                          "http://example.com/blog",
                          "http://news.example.org/latest"}},
        {"/about",       {"http://example.com/",
                          "http://example.com/contact"}},
        {"/blog",        {"http://example.com/blog/post-1",
                          "http://example.com/blog/post-2"}},
        {"/contact",     {}},
        {"/blog/post-1", {"http://example.com/blog",
                          "http://shop.example.net/widget"}},
        {"/blog/post-2", {"http://example.com/blog",
                          "http://news.example.org/latest"}},
    }},
    {"news.example.org", {
        {"/latest",      {"http://news.example.org/archive",
                          "http://news.example.org/about",
                          "http://example.com/"}},
        {"/archive",     {"http://news.example.org/latest",
                          "http://news.example.org/page-2"}},
        {"/about",       {"http://news.example.org/"}},
        {"/",            {"http://news.example.org/latest",
                          "http://news.example.org/archive"}},
        {"/page-2",      {}},
    }},
    {"shop.example.net", {
        {"/widget",      {"http://shop.example.net/",
                          "http://shop.example.net/cart"}},
        {"/",            {"http://shop.example.net/widget",
                          "http://shop.example.net/checkout"}},
        {"/cart",        {"http://shop.example.net/",
                          "http://shop.example.net/checkout"}},
        {"/checkout",    {}},
    }},
};

// Serve one HTTP connection: look up pages in kGraph and respond.
static void serve_conn(http::endpoint ep) {
    http::request req;
    while (ep.requests >> req) {
        // Strip port suffix from Host header (e.g. "example.com:54321" → "example.com").
        auto host_hdr = req.header("Host");
        auto colon = host_hdr.find(':');
        std::string host = (colon != std::string::npos)
                         ? host_hdr.substr(0, colon) : host_hdr;

        auto hit = kGraph.find(host);
        if (hit == kGraph.end()) { req.respond << http::response{404, {}, {}}; continue; }
        auto page = hit->second.find(req.url);
        if (page == hit->second.end()) { req.respond << http::response{404, {}, {}}; continue; }

        std::string body;
        for (const auto& link : page->second) body += link + "\n";
        req.respond << http::response{
            200,
            {{"Content-Type", "text/plain"}},
            bytes(body.begin(), body.end())
        };
    }
}

// ---------------------------------------------------------------------------
// Crawler types
// ---------------------------------------------------------------------------

struct FetchReq    { std::string url; int depth; };
struct CrawlResult { std::string url; int depth; std::vector<std::string> links; };

// Parse "http://host[:port]/path" — returns false on malformed input.
static bool parse_url(const std::string& url,
                      std::string& host, uint16_t& port, std::string& path) {
    if (url.compare(0, 7, "http://") != 0) return false;
    auto rest = url.substr(7);
    auto slash = rest.find('/');
    auto hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = static_cast<uint16_t>(std::stoi(hostport.substr(colon + 1)));
    } else {
        host = hostport;
        port = 80;
    }
    return !host.empty();
}

static std::string url_host(const std::string& url) {
    std::string h, p; uint16_t port;
    return parse_url(url, h, port, p) ? h : url;
}

// Hand-rolled HTTP/1.0 GET.
//
// Connects to 127.0.0.1:server_port, sends a GET with the virtual hostname
// in the Host header (so the in-process server routes correctly), and reads
// the response.  Returns the response body on HTTP 200, or empty on failure.
//
// Using HTTP/1.0 (Connection: close) avoids keep-alive tracking and chunked
// transfer encoding.  The full response is read until the connection closes.
static std::string raw_get(uint16_t server_port, const std::string& host,
                           const std::string& path) {
    try {
        auto conn = net::dial("127.0.0.1", server_port);

        // Build request.
        std::string req =
            "GET " + path + " HTTP/1.0\r\n"
            "Host: " + host + "\r\n"
            "Connection: close\r\n"
            "\r\n";
        conn.output << bytes(req.begin(), req.end());
        conn.output = {};  // close write side; sends FIN

        // Read full response.
        std::string response;
        for (;;) {
            auto rr = io::call_source(conn.source, 4096);
            bytes chunk;
            if (!(rr >> chunk)) break;
            response.append(chunk.begin(), chunk.end());
        }

        // Split headers / body on the first blank line.
        auto sep = response.find("\r\n\r\n");
        if (sep == std::string::npos) return {};

        // Check status (first line must start with "HTTP/... 200").
        auto first_line_end = response.find("\r\n");
        if (first_line_end == std::string::npos) return {};
        auto status_line = response.substr(0, first_line_end);
        if (status_line.find(" 200 ") == std::string::npos &&
            status_line.rfind(" 200") != status_line.size() - 4) return {};

        return response.substr(sep + 4);
    } catch (...) {
        return {};
    }
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------
//
// Each worker maintains per-host last-fetch timestamps for rate limiting.

static void run_worker(int id, reader<FetchReq> in, writer<CrawlResult> out,
                       uint16_t server_port) {
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_fetch;
    constexpr auto kMinGap = 100ms;

    FetchReq req;
    while (in >> req) {
        // Per-host rate limiting.
        auto host = url_host(req.url);
        auto now  = std::chrono::steady_clock::now();
        if (auto it = last_fetch.find(host); it != last_fetch.end()) {
            auto gap = now - it->second;
            if (gap < kMinGap) sleep(kMinGap - gap);
        }

        // Fetch and parse links.
        std::string fetch_host, path;
        uint16_t dummy_port;
        std::vector<std::string> links;
        if (parse_url(req.url, fetch_host, dummy_port, path)) {
            auto body = raw_get(server_port, fetch_host, path);
            std::string line;
            for (char c : body) {
                if (c == '\n') {
                    if (!line.empty()) { links.push_back(line); line.clear(); }
                } else {
                    line += c;
                }
            }
            if (!line.empty()) links.push_back(line);
        }

        last_fetch[host] = std::chrono::steady_clock::now();
        printf("  [worker %d] depth=%d  links=%-2d  %s\n",
               id, req.depth, static_cast<int>(links.size()), req.url.c_str());

        out << CrawlResult{req.url, req.depth, std::move(links)};
    }
    // in died (frontier closed) → dropping out eventually closes results.
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    setbuf(stdout, nullptr);

    // Port channel: server imp sends its bound port to the coordinator.
    chan<uint16_t> port_ch;
    // Stop channel: coordinator drops stop_ch.w when the crawl is done.
    chan<poke_t> stop_ch;

    // Server imp: bind, report port, accept connections until stop fires.
    spawn([port_w = std::move(port_ch.w),
           stop_r = std::move(stop_ch.r)] () mutable {
        auto srv = http::serve(0);
        port_w << srv.port;
        port_w = {};

        http::endpoint ep;
        for (;;) {
            switch (prialt(srv.endpoints >> ep, ~stop_r)) {
            case 0:   // new connection
                spawn([ep = std::move(ep)] () mutable { serve_conn(std::move(ep)); });
                break;
            case ~1:  // stop_ch.w dropped → exit
                return;
            default:
                return;
            }
        }
    });

    // Coordinator imp: wait for port, run the crawl, signal server to stop.
    spawn([port_r = std::move(port_ch.r),
           stop_w = std::move(stop_ch.w)] () mutable {
        uint16_t port;
        port_r >> port;
        port_r = {};

        constexpr int kWorkers   = 4;
        constexpr int kMaxDepth  = 3;
        const std::string kSeed  = "http://example.com/";

        printf("Web Crawler — in-process server on port %d, "
               "%d workers, max depth %d\n\n", port, kWorkers, kMaxDepth);

        chan<FetchReq>    frontier(32);
        chan<CrawlResult> results;

        for (int i = 0; i < kWorkers; ++i) {
            spawn([i,
                   in  = frontier.r.copy(),
                   out = results.w.copy(),
                   port] () mutable {
                run_worker(i, std::move(in), std::move(out), port);
            });
        }
        frontier.r = {};  // coordinator doesn't consume the frontier
        results.w  = {};  // coordinator doesn't produce results

        std::unordered_set<std::string> seen;
        int in_flight     = 0;
        int total_crawled = 0;
        int total_skipped = 0;

        auto enqueue = [&](const std::string& url, int depth) {
            if (depth > kMaxDepth) return;
            if (!seen.insert(url).second) { ++total_skipped; return; }
            frontier.w << FetchReq{url, depth};
            ++in_flight;
        };

        enqueue(kSeed, 0);

        CrawlResult res;
        while (results.r >> res) {
            ++total_crawled;
            --in_flight;
            for (const auto& link : res.links) enqueue(link, res.depth + 1);
            if (in_flight == 0) {
                // Quiescent: drop frontier → workers exit → results channel closes.
                frontier.w = {};
            }
        }

        printf("\nCrawl complete: %d pages fetched, %d duplicates skipped.\n",
               total_crawled, total_skipped);

        // stop_w drops here → ~stop_r fires in server imp → it exits.
    });

    schedule();

    // Explicitly shut down subsystems before static-singleton destructors run.
    // net::dial uses io::resolve (blocking pool) for DNS; those threads are
    // daemon threads that call std::terminate via std::thread::~thread() if
    // left joinable when main() exits without an explicit shutdown.
    csp::shutdown_runtime();
}
