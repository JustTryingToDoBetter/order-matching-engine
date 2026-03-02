#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "engine_pool.hpp"

namespace {

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

inline bool isResting(AddResult addResult) {
    return addResult == AddResult::FullyRested || addResult == AddResult::PartiallyRested;
}

inline void pruneClosedOrders(LiveSet& live, TradeSink& sink) {
    for (OrderId id : sink.closedOrderIds) {
        live.remove(id);
    }
    sink.clearClosedOrderIds();
}

struct WorkloadStats {
    std::size_t engineLive = 0;
    std::size_t benchLive = 0;
    long long trades = 0;
    long long totalQty = 0;
};

WorkloadStats runDeterministicWorkload(int64_t ops, uint32_t seed) {
    constexpr int addPct = 60;
    constexpr int cancelPct = 25;
    constexpr int replacePct = 15;

    const int maxId = static_cast<int>(ops) + 10;
    OrderBookPool book(/*expected_orders=*/300000, /*max_order_id=*/maxId);
    TradeSink sink;
    LiveSet live(maxId);
    std::mt19937 rng(seed);

    OrderId nextId = 1;

    for (int64_t i = 0; i < ops; ++i) {
        int roll = randPercent(rng);

        if (roll <= addPct) {
            Side side = (randBounded(rng, 2) == 0) ? Side::buy : Side::sell;
            Price price = randPrice(rng);
            int qty = static_cast<int>(randBounded(rng, 10)) + 1;
            OrderId id = nextId++;

            AddResult addResult = book.matchIncoming(Order{id, side, price, qty}, sink);
            if (isResting(addResult)) {
                live.add(id);
            }
        } else if (roll <= addPct + cancelPct) {
            if (!live.empty()) {
                OrderId id = live.pick(rng);
                if (book.cancel(id)) {
                    live.remove(id);
                } else {
                    live.remove(id);
                }
            }
        } else if (roll <= addPct + cancelPct + replacePct) {
            if (!live.empty()) {
                OrderId id = live.pick(rng);
                Price newPrice = randPrice(rng);
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

    WorkloadStats stats;
    stats.engineLive = book.liveOrders();
    stats.benchLive = live.ids.size();
    stats.trades = sink.tradeCount;
    stats.totalQty = sink.totalQty;
    return stats;
}

[[noreturn]] void fail(const std::string& name, const std::string& message) {
    std::cout << "FAIL: " << name << " - " << message << "\n";
    std::exit(1);
}

void expect(const std::string& name, bool condition, const std::string& message) {
    if (!condition) {
        fail(name, message);
    }
}

void scenario_fully_crossing_incoming_does_not_rest() {
    const std::string name = "fully crossing incoming order does not rest";

    OrderBookPool book(/*expected_orders=*/16, /*max_order_id=*/128);
    TradeSink sink;

    book.matchIncoming(Order{10, Side::sell, 1000, 5}, sink);
    expect(name, book.liveOrders() == 1, "seed resting order must be live");

    book.matchIncoming(Order{20, Side::buy, 1005, 5}, sink);

    expect(name, sink.tradeCount == 1, "must emit one trade");
    expect(name, sink.totalQty == 5, "trade qty must equal crossed quantity");
    expect(name, book.liveOrders() == 0, "fully crossing incoming order must not rest");
    expect(name, !book.cancel(20), "incoming taker id must never appear live");

    std::cout << "PASS: " << name << "\n";
}

void scenario_partial_fill_remainder_rests_and_is_indexed() {
    const std::string name = "partial fill leaves remainder resting/indexed";

    OrderBookPool book(/*expected_orders=*/16, /*max_order_id=*/128);
    TradeSink sink;

    book.matchIncoming(Order{11, Side::sell, 1000, 10}, sink);
    book.matchIncoming(Order{21, Side::buy, 1005, 6}, sink);

    expect(name, sink.tradeCount == 1, "must emit one trade");
    expect(name, sink.totalQty == 6, "must fill six shares/contracts");
    expect(name, book.liveOrders() == 1, "maker remainder must remain live");
    expect(name, !book.cancel(21), "fully filled incoming taker must not be indexed/live");
    expect(name, book.cancel(11), "remainder maker must be cancellable once");
    expect(name, !book.cancel(11), "maker cancel must fail after removal");

    std::cout << "PASS: " << name << "\n";
}

void scenario_cancel_remove_once_second_fails() {
    const std::string name = "cancel removes once and second cancel fails";

    OrderBookPool book(/*expected_orders=*/16, /*max_order_id=*/128);
    TradeSink sink;

    book.matchIncoming(Order{30, Side::buy, 995, 7}, sink);

    expect(name, book.liveOrders() == 1, "resting order must be live before cancel");
    expect(name, book.cancel(30), "first cancel must succeed");
    expect(name, book.liveOrders() == 0, "book must have no live orders after cancel");
    expect(name, !book.cancel(30), "second cancel must fail");

    std::cout << "PASS: " << name << "\n";
}

void scenario_replace_cancel_reinsert_and_index_points_to_new_live_slot() {
    const std::string name = "replace has cancel+reinsert semantics with live index update";

    OrderBookPool book(/*expected_orders=*/16, /*max_order_id=*/128);
    TradeSink sink;

    book.matchIncoming(Order{40, Side::buy, 995, 10}, sink);
    book.matchIncoming(Order{41, Side::sell, 1000, 4}, sink);
    expect(name, book.liveOrders() == 2, "both seed orders must be live before replace");

    expect(name, book.replace(40, 1001, 6, sink).success, "replace must succeed for live id");

    expect(name, sink.tradeCount == 1, "replace-generated incoming order must trade once");
    expect(name, sink.totalQty == 4, "replace-generated incoming order must trade expected qty");
    expect(name, book.liveOrders() == 1, "replace should remove old order, trade, and rest remainder");

    expect(name, book.cancel(40), "id must point to newly inserted live remainder");
    expect(name, !book.cancel(40), "second cancel must fail after removing replaced order");
    expect(name, !book.cancel(41), "maker fully filled by replace must no longer be live");

    std::cout << "PASS: " << name << "\n";
}

void scenario_deterministic_workload_liveset_sync() {
    const std::string name = "deterministic workload keeps benchmark liveset synced";

    constexpr int64_t ops = 50'000;
    constexpr uint32_t seed = 12345;

    WorkloadStats first = runDeterministicWorkload(ops, seed);
    WorkloadStats second = runDeterministicWorkload(ops, seed);

    expect(name, first.engineLive == first.benchLive, "engine and benchmark live counts must match");
    expect(name, second.engineLive == second.benchLive, "engine and benchmark live counts must match on rerun");
    expect(name, first.trades == second.trades, "trade count must be deterministic for fixed seed");
    expect(name, first.totalQty == second.totalQty, "filled quantity must be deterministic for fixed seed");

    std::cout << "PASS: " << name << "\n";
}

} // namespace

int main() {
    scenario_fully_crossing_incoming_does_not_rest();
    scenario_partial_fill_remainder_rests_and_is_indexed();
    scenario_cancel_remove_once_second_fails();
    scenario_replace_cancel_reinsert_and_index_points_to_new_live_slot();
    scenario_deterministic_workload_liveset_sync();

    std::cout << "PASS: all deterministic engine scenarios\n";
    return 0;
}
