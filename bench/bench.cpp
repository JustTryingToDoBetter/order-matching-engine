#include "engine_pool.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

static constexpr Price MID = 1000;
static constexpr int SPREAD = 50;

static inline uint32_t nextRand(std::mt19937& rng) {
    return rng();
}

static inline uint32_t randBounded(std::mt19937& rng, uint32_t boundExclusive) {
    return nextRand(rng) % boundExclusive;
}

static inline int randPercent(std::mt19937& rng) {
    return static_cast<int>(randBounded(rng, 100)) + 1;
}

static inline Price randPrice(std::mt19937& rng) {
    return MID - SPREAD + static_cast<Price>(randBounded(rng, static_cast<uint32_t>(2 * SPREAD + 1)));
}

struct BenchConfig {
    std::string mode = "maintenance";
    int64_t ops = 5'000'000;
    uint32_t seed = 12345;

    int addPct = 60;
    int cancelPct = 25;
    int replacePct = 15;
    int crossBiasPct = 80;
};

struct LiveSet {
    std::vector<OrderId> ids;
    std::vector<int> pos;

    explicit LiveSet(int maxId) : pos(static_cast<std::size_t>(maxId) + 1, -1) {
        ids.reserve(static_cast<std::size_t>(maxId) + 1);
    }

    inline bool contains(OrderId id) const {
        return id >= 0 && static_cast<std::size_t>(id) < pos.size() && pos[static_cast<std::size_t>(id)] != -1;
    }

    inline void add(OrderId id) {
        if (id < 0 || static_cast<std::size_t>(id) >= pos.size()) return;
        if (pos[static_cast<std::size_t>(id)] != -1) return;

        pos[static_cast<std::size_t>(id)] = static_cast<int>(ids.size());
        ids.push_back(id);
    }

    inline void remove(OrderId id) {
        if (id < 0 || static_cast<std::size_t>(id) >= pos.size()) return;

        int p = pos[static_cast<std::size_t>(id)];
        if (p == -1) return;

        int last = static_cast<int>(ids.size()) - 1;
        if (p != last) {
            OrderId swapId = ids[static_cast<std::size_t>(last)];
            ids[static_cast<std::size_t>(p)] = swapId;
            pos[static_cast<std::size_t>(swapId)] = p;
        }

        ids.pop_back();
        pos[static_cast<std::size_t>(id)] = -1;
    }

    inline bool empty() const { return ids.empty(); }

    inline OrderId pick(std::mt19937& rng) const {
        return ids[randBounded(rng, static_cast<uint32_t>(ids.size()))];
    }
};

static inline bool isResting(AddResult addResult) {
    return addResult == AddResult::FullyRested || addResult == AddResult::PartiallyRested;
}

static inline void pruneClosedOrders(LiveSet& live, TradeSink& sink) {
    for (OrderId id : sink.closedOrderIds) {
        live.remove(id);
    }
    sink.clearClosedOrderIds();
}

static BenchConfig parseArgs(int argc, char** argv) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) return "";
            return argv[++i];
        };

        if (arg == "--mode") cfg.mode = next();
        else if (arg == "--ops") cfg.ops = std::stoll(next());
        else if (arg == "--seed") cfg.seed = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--cross") cfg.crossBiasPct = std::stoi(next());
        else if (arg == "--add") cfg.addPct = std::stoi(next());
        else if (arg == "--cancel") cfg.cancelPct = std::stoi(next());
        else if (arg == "--replace") cfg.replacePct = std::stoi(next());
    }
    return cfg;
}

int main(int argc, char** argv) {
    BenchConfig cfg = parseArgs(argc, argv);
    if (cfg.addPct + cfg.cancelPct + cfg.replacePct != 100) {
        std::cerr << "ERROR: add+cancel+replace must sum to 100\n";
        return 1;
    }

    const int maxId = static_cast<int>(cfg.ops) + 10;
    OrderBookPool book(/*expected_orders=*/300000, /*max_order_id=*/maxId);
    TradeSink sink;
    std::mt19937 rng(cfg.seed);
    LiveSet live(maxId);

    OrderId nextId = 1;
    auto t0 = std::chrono::steady_clock::now();

    for (int64_t i = 0; i < cfg.ops; ++i) {
        int roll = randPercent(rng);

        if (roll <= cfg.addPct) {
            Side side = (randBounded(rng, 2) == 0) ? Side::buy : Side::sell;
            Price price = randPrice(rng);
            if (cfg.mode == "match" && randPercent(rng) <= cfg.crossBiasPct) {
                price = (side == Side::buy) ? (MID + SPREAD) : (MID - SPREAD);
            }

            int qty = static_cast<int>(randBounded(rng, 10)) + 1;
            OrderId id = nextId++;
            AddResult addResult = book.matchIncoming(Order{id, side, price, qty}, sink);
            if (isResting(addResult)) {
                live.add(id);
            }
        } else if (roll <= cfg.addPct + cfg.cancelPct) {
            if (!live.empty()) {
                OrderId id = live.pick(rng);
                if (book.cancel(id)) {
                    live.remove(id);
                } else {
                    live.remove(id);
                }
            }
        } else {
            if (!live.empty()) {
                OrderId id = live.pick(rng);
                Price newPrice = randPrice(rng);
                if (cfg.mode == "match" && randPercent(rng) <= cfg.crossBiasPct) {
                    newPrice = MID;
                }

                int newQty = static_cast<int>(randBounded(rng, 10)) + 1;
                ReplaceResult replaceResult = book.replace(id, newPrice, newQty, sink);
                if (!replaceResult.success || !replaceResult.rested()) {
                    live.remove(id);
                } else if (!live.contains(id)) {
                    live.add(id);
                }
            }
        }

        pruneClosedOrders(live, sink);
    }

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

    return 0;
}
