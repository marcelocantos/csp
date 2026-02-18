#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Share a source across multiple subscribers. Each read from the returned
// channel creates a new subscription that immediately delivers the current
// value (if any) then subsequent updates. Each subscriber gets a dedicated
// latch microthread with independent backpressure: a slow subscriber sees
// latest-value semantics (intermediate values overwritten), while fast
// subscribers see every value.
template <typename T>
reader<reader<T>> share(reader<T> source) {
    chan<reader<T>> sub_ch;
    reader<reader<T>> result = std::move(sub_ch.r);

    csp::spawn([source = std::move(source),
                sub_out = std::move(sub_ch.w)]() mutable {
        internal::descr("share");

        std::vector<writer<T>> feeds;
        T latest;
        bool has_value = false;
        bool accepting = true;

        for (;;) {
            T new_val;

            if (accepting) {
                chan<T> feed;
                chan<T> ext;
                switch (csp::alt(source >> new_val,
                                 sub_out << std::move(ext.r))) {
                case 0:  // Source value.
                    latest = std::move(new_val);
                    has_value = true;
                    break;
                case 1:  // New subscriber took ext.r.
                    // Spawn per-subscriber latch.
                    csp::spawn([fr = std::move(feed.r),
                                out = std::move(ext.w)]() mutable {
                        T val;
                        if (!(fr >> val)) return;
                        for (;;) {
                            T update;
                            switch (csp::alt(fr >> update, out << val)) {
                            case 0:  // New value overwrites pending.
                                val = std::move(update);
                                break;
                            case 1:  // Subscriber read — wait for next.
                                if (!(fr >> val)) {
                                    return;
                                }
                                break;
                            case ~0:  // Feed dead — deliver last value.
                                out << std::move(val);
                                return;
                            default:  // Subscriber dead.
                                return;
                            }
                        }
                    });
                    // Deliver current value through feed.
                    if (has_value) feed.w << latest;
                    feeds.push_back(std::move(feed.w));
                    continue;
                case ~0:  // Source died.
                    return;
                default:  // Subscription channel dead.
                    accepting = false;
                    if (feeds.empty()) return;
                    continue;
                }
            } else {
                if (feeds.empty()) return;
                if (!(source >> new_val)) return;
                latest = std::move(new_val);
                has_value = true;
            }

            // Broadcast to all live feeds.
            auto it = feeds.begin();
            while (it != feeds.end()) {
                if (!(*it << latest)) {
                    it = feeds.erase(it);
                } else {
                    ++it;
                }
            }
        }
    });

    return result;
}

}
