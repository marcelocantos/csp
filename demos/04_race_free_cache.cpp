// 04_race_free_cache.cpp — Concurrent key-value store without shared_mutex
//
// A server imp owns an unordered_map exclusively. Clients send request structs
// that include a reply channel; the server processes one request at a time and
// sends the result back. No shared_mutex, no torn reads, no RCU.
//
// In std C++ you'd protect the map with std::shared_mutex (readers in shared
// mode, writers in exclusive mode), carefully ensuring every access is covered.
// Here the map is never touched by more than one imp — the channel is both the
// lock and the queue.

#include "csp.h"

#include <cstdio>
#include <string>
#include <unordered_map>

using namespace csp;

enum class Op { Get, Put, Del };

struct CacheReq {
    Op          op;
    std::string key;
    std::string val;           // used by Put
    writer<std::string> reply; // "" means "not found" / "ok" / "deleted"
};

int main() {
    spawn([]{
        auto [req_w, req_r] = chan<CacheReq>{};

        // Server imp — sole owner of the map.
        spawn([r = std::move(req_r)]{
            std::unordered_map<std::string, std::string> store;
            for (CacheReq req; r >> req;) {
                switch (req.op) {
                case Op::Get: {
                    auto it = store.find(req.key);
                    req.reply << (it != store.end() ? it->second : "");
                    break;
                }
                case Op::Put:
                    store[req.key] = req.val;
                    req.reply << "ok";
                    break;
                case Op::Del:
                    store.erase(req.key);
                    req.reply << "deleted";
                    break;
                }
            }
        });

        // Helper — sends a request and waits for reply.
        auto call = [](writer<CacheReq> const& w, Op op,
                       std::string key, std::string val = {}) {
            auto [rw, rr] = chan<std::string>{};
            w << CacheReq{op, std::move(key), std::move(val), std::move(rw)};
            return rr.read();
        };

        // Client imps — each holds its own copy of the request channel.
        spawn([w = req_w.copy(), call]{
            call(w, Op::Put, "color", "blue");
            printf("  client A: set color=blue\n");
            printf("  client A: get color -> %s\n",
                   call(w, Op::Get, "color").c_str());
        });

        spawn([w = req_w.copy(), call]{
            call(w, Op::Put, "shape", "circle");
            printf("  client B: set shape=circle\n");
            call(w, Op::Del, "shape");
            printf("  client B: del shape\n");
            printf("  client B: get shape -> '%s' (empty = deleted)\n",
                   call(w, Op::Get, "shape").c_str());
        });
    });

    schedule();
}
