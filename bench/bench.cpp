#include "engine_pool.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static constexpr Price MID = 1000;
static constexpr int   SPREAD = 50;  // prices in [MID-SPREAD, MID+SPREAD]

// Deterministic tick generator within band.
static inline uint32_t nextRand(std::mt19937& rng) {
    // Raw mt19937 output: deterministic for same seed/op-count and cheaper than
    // constructing distribution objects in hot paths.
    return rng();
}

static inline uint32_t randBounded(std::mt19937& rng, uint32_t boundExclusive) {
    return nextRand(rng) % boundExclusive;
}

static inline int randPercent(std::mt19937& rng) {
    return (int)randBounded(rng, 100) + 1; // [1, 100]
}

static inline Price randPrice(std::mt19937& rng) {
    return MID - SPREAD + (Price)randBounded(rng, (uint32_t)(2 * SPREAD + 1));
}

struct BenchConfig {
    std::string mode = "maintenance"; // "maintenance" or "match"
    int64_t ops = 5'000'000;
    uint32_t seed = 12345;

    // Operation mix (percentages must sum to 100)
    int addPct = 60;
    int cancelPct = 25;
    int replacePct = 15;

    // For match-heavy mode: widen crossing chance
    int crossBiasPct = 80; // % of new orders that are priced to cross the spread
};

struct LiveSet {
    // We maintain a dense array of live IDs + id->pos for O(1) remove.
    std::vector<OrderId> ids;
    std::vector<int> pos; // pos[id] = index in ids, or -1 if not live

    explicit LiveSet(int maxId) : pos(maxId + 1, -1) {
        ids.reserve(maxId + 1);
    }

    inline bool contains(OrderId id) const {
        return id >= 0 && id < (OrderId)pos.size() && pos[id] != -1;
    }

    inline void add(OrderId id) {
        if ((size_t)id >= pos.size()) return; // safety
        if (pos[id] != -1) return;
        pos[id] = (int)ids.size();
        ids.push_back(id);
    }

    inline void remove(OrderId id) {
        if ((size_t)id >= pos.size()) return;
        int p = pos[id];
        if (p == -1) return;
        int last = (int)ids.size() - 1;
        if (p != last) {
            OrderId swapId = ids[last];
            ids[p] = swapId;
            pos[swapId] = p;
        }
        ids.pop_back();
        pos[id] = -1;
    }

    inline bool empty() const { return ids.empty(); }

    inline OrderId pick(std::mt19937& rng) const {
        return ids[randBounded(rng, (uint32_t)ids.size())];
    }
};


#ifndef NDEBUG
static inline void reconcileLiveSet(const OrderBookPool& book, LiveSet& live) {
    for (std::size_t idx = live.ids.size(); idx > 0; --idx) {
        const OrderId id = live.ids[idx - 1];
        if (!book.isLive(id)) {
            live.remove(id);
        }
    }
}

static inline void assertBenchInvariant(const OrderBookPool& book,
                                        const LiveSet& live,
                                        int64_t opIndex,
                                        const char* opType,
                                        OrderId orderId) {
    const auto engineLive = book.liveOrders();
    const auto indexLive = book.indexLiveCount();
    const auto benchLive = live.ids.size();

    if (engineLive != indexLive || engineLive != benchLive) {
        std::cerr << "Invariant failure at op=" << opIndex
                  << " type=" << opType
                  << " order_id=" << orderId
                  << " | engineLive=" << engineLive
                  << " indexLive=" << indexLive
                  << " benchLive=" << benchLive
                  << "\n";
        assert(false && "benchmark invariant failure");
    }
}
#endif

static BenchConfig parseArgs(int argc, char** argv) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) return "";
            return argv[++i];
        };

        if (a == "--mode") cfg.mode = next("--mode");
        else if (a == "--ops") cfg.ops = std::stoll(next("--ops"));
        else if (a == "--seed") cfg.seed = (uint32_t)std::stoul(next("--seed"));
        else if (a == "--cross") cfg.crossBiasPct = std::stoi(next("--cross"));
        else if (a == "--add") cfg.addPct = std::stoi(next("--add"));
        else if (a == "--cancel") cfg.cancelPct = std::stoi(next("--cancel"));
        else if (a == "--replace") cfg.replacePct = std::stoi(next("--replace"));
    }
    return cfg;
}

int main(int argc, char** argv) {
    BenchConfig cfg = parseArgs(argc, argv);

    // Sanity on mix
    if (cfg.addPct + cfg.cancelPct + cfg.replacePct != 100) {
        std::cerr << "ERROR: add+cancel+replace must sum to 100\n";
        return 1;
    }

    // Pre-size IDs to avoid reallocations & make determinism tight.
    int maxId = (int)cfg.ops + 10;

    OrderBookPool book(/*expected_orders=*/ 300000, /*max_order_id=*/ maxId); // reserve for speed
    TradeSink sink;

    std::mt19937 rng(cfg.seed);
    // NOTE: sequence differs from uniform_int_distribution-based mapping, but
    // remains deterministic for fixed seed and operation count.

    LiveSet live(maxId);

    OrderId nextId = 1;

    auto t0 = std::chrono::steady_clock::now();

    for (int64_t i = 0; i < cfg.ops; ++i) {
        int r = randPercent(rng);

        if (r <= cfg.addPct) {
            Side side = (randBounded(rng, 2) == 0) ? Side::buy : Side::sell;

            Price p = randPrice(rng);

            if (cfg.mode == "match") {
                // Bias the price so we more often cross:
                // buys skew high, sells skew low.
                // crossBiasPct controls how often we do this.
                if (randPercent(rng) <= cfg.crossBiasPct) {
                    if (side == Side::buy) p = MID + SPREAD;   // aggressive buy
                    else p = MID - SPREAD;                    // aggressive sell
                }
            }

            int qty = (int)randBounded(rng, 10) + 1;
            OrderId id = nextId++;
            opOrderId = id;

            book.matchIncoming(Order{id, side, p, qty}, sink);
            if (book.isLive(id)) {
                live.add(id);
            }

        } else if (r <= cfg.addPct + cfg.cancelPct) {
            opType = "cancel";
            if (!live.empty()) {
                OrderId id = live.pick(rng);
                opOrderId = id;
                if (book.cancel(id)) {
                    live.remove(id);
                } else {
                    live.remove(id);
                }
            }

        } else {
            opType = "replace";
            if (!live.empty()) {
                OrderId id = live.pick(rng);
                opOrderId = id;
                Price newP = randPrice(rng);
                if (cfg.mode == "match") {
                    // keep replacements somewhat aggressive too
                    if (randPercent(rng) <= cfg.crossBiasPct) {
                        // we don't know side here without querying engine; keep neutral-ish:
                        newP = MID; // tends to interact
                    }
                }
                int newQ = (int)randBounded(rng, 10) + 1;

                if (!book.replace(id, newP, newQ, sink)) {
                    live.remove(id);
                } else if (!book.isLive(id)) {
                    live.remove(id);
                }
            }
        }

#ifndef NDEBUG
        if (opType != nullptr) {
            reconcileLiveSet(book, live);
            assertBenchInvariant(book, live, i, opType, opOrderId);
        }
#endif
    }

#ifndef NDEBUG
    reconcileLiveSet(book, live);
    assertBenchInvariant(book, live, cfg.ops, "final", -1);
#endif

    auto t1 = std::chrono::steady_clock::now();
    std::chrono::duration<double> dt = t1 - t0;

    std::cout << "Mode: " << cfg.mode << "\n";
    std::cout << "Ops: " << cfg.ops << "\n";
    std::cout << "Seconds: " << dt.count() << "\n";
    std::cout << "Ops/sec: " << (cfg.ops / dt.count()) << "\n";
    std::cout << "Trades: " << sink.tradeCount << "\n";
    std::cout << "Total filled qty: " << sink.totalQty << "\n";
    std::cout << "Live orders (engine): " << book.liveOrders() << "\n";
    std::cout << "Live orders (bench-set): " << live.ids.size() << "\n";
}
