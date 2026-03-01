#pragma once

#include <algorithm>
#include <list>
#include <unordered_map>
#include <vector>
#include <limits>

enum class Side { buy, sell };

using OrderId = int;
using Price   = int;   // integer ticks

// ---------------- Trade Sink ----------------

struct TradeSink {
    long long tradeCount = 0;
    long long totalQty   = 0;

    inline void onTrade(int qty, Price price, OrderId taker, OrderId maker) {
        (void)price; (void)taker; (void)maker;
        tradeCount++;
        totalQty += qty;
    }
};

// ---------------- Order ----------------

struct Order {
    OrderId id;
    Side side;
    Price price;
    int qty;
};

// ---------------- Price Level ----------------

struct PriceLevel {
    std::list<Order> fifo;
    int totalQuantity = 0;
};

// ---------------- Engine ----------------

// Bounded tick band for benchmark.
// Adjust if you widen price generator.
constexpr Price MIN_TICK = 900;
constexpr Price MAX_TICK = 1100;
constexpr int   NUM_LEVELS = MAX_TICK - MIN_TICK + 1;

using OrderIt = std::list<Order>::iterator; // iterator to order in price level FIFO

struct OrderRef {
    Side side;
    Price price;
    OrderIt it;
};

// Simple array-based order book with O(1) access to price levels and lazy best price tracking.
class OrderBook {
public:
    OrderBook() {
        bidLevels.resize(NUM_LEVELS); // low -> high
        askLevels.resize(NUM_LEVELS); // high -> low
    }
    // Add, cancel, replace, match functions defined below.
    bool addToBook(const Order& o) {
        if (o.qty <= 0) return false;
        if (o.price < MIN_TICK || o.price > MAX_TICK) return false;
        if (index.find(o.id) != index.end()) return false;

        int idx = toIndex(o.price);

        if (o.side == Side::buy) {
            auto& level = bidLevels[idx];
            level.fifo.push_back(o);
            level.totalQuantity += o.qty;

            auto it = std::prev(level.fifo.end());
            index.emplace(o.id, OrderRef{o.side, o.price, it});

            if (bestBidIdx < idx) bestBidIdx = idx;
        } else {
            auto& level = askLevels[idx];
            level.fifo.push_back(o);
            level.totalQuantity += o.qty;

            auto it = std::prev(level.fifo.end());
            index.emplace(o.id, OrderRef{o.side, o.price, it});

            if (bestAskIdx > idx) bestAskIdx = idx;
        }

        return true;
    }

    bool cancel(OrderId id) {
        auto itRef = index.find(id);
        if (itRef == index.end()) return false;

        Price price = itRef->second.price;
        Side side   = itRef->second.side;
        OrderIt it  = itRef->second.it;

        int idx = toIndex(price);

        auto& level = (side == Side::buy) ? bidLevels[idx] : askLevels[idx];

        level.totalQuantity -= it->qty;
        level.fifo.erase(it);
        index.erase(itRef);

        // If level empty, update best pointers lazily
        if (level.fifo.empty()) {
            if (side == Side::buy && idx == bestBidIdx)
                updateBestBid();
            if (side == Side::sell && idx == bestAskIdx)
                updateBestAsk();
        }

        return true;
    }

    bool replace(OrderId id, Price newPrice, int newQty, TradeSink& sink) {
        auto itRef = index.find(id);
        if (itRef == index.end()) return false;

        Side side = itRef->second.side;

        cancel(id);

        matchIncoming({id, side, newPrice, newQty}, sink);
        return true;
    }

    void matchIncoming(Order incoming, TradeSink& sink) {
        if (incoming.qty <= 0) return;
        if (incoming.price < MIN_TICK || incoming.price > MAX_TICK) return;

        if (incoming.side == Side::buy) {
            while (incoming.qty > 0 && bestAskIdx <= bestBidBound()) {
                if (bestAskIdx > toIndex(incoming.price)) break;

                auto& level = askLevels[bestAskIdx];

                while (incoming.qty > 0 && !level.fifo.empty()) {
                    auto makerIt = level.fifo.begin();
                    Order& maker = *makerIt;

                    int fill = std::min(incoming.qty, maker.qty);

                    maker.qty -= fill;
                    incoming.qty -= fill;
                    level.totalQuantity -= fill;

                    sink.onTrade(fill, fromIndex(bestAskIdx), incoming.id, maker.id);

                    if (maker.qty == 0) {
                        index.erase(maker.id);
                        level.fifo.erase(makerIt);
                    }
                }

                if (level.fifo.empty())
                    updateBestAsk();
                else
                    break;
            }

            if (incoming.qty > 0)
                addToBook(incoming);
        }
        else {
            while (incoming.qty > 0 && bestBidIdx >= bestAskBound()) {
                if (bestBidIdx < toIndex(incoming.price)) break;

                auto& level = bidLevels[bestBidIdx];

                while (incoming.qty > 0 && !level.fifo.empty()) {
                    auto makerIt = level.fifo.begin();
                    Order& maker = *makerIt;

                    int fill = std::min(incoming.qty, maker.qty);

                    maker.qty -= fill;
                    incoming.qty -= fill;
                    level.totalQuantity -= fill;

                    sink.onTrade(fill, fromIndex(bestBidIdx), incoming.id, maker.id);

                    if (maker.qty == 0) {
                        index.erase(maker.id);
                        level.fifo.erase(makerIt);
                    }
                }

                if (level.fifo.empty())
                    updateBestBid();
                else
                    break;
            }

            if (incoming.qty > 0)
                addToBook(incoming);
        }
    }

    size_t liveOrders() const {
        return index.size();
    }

private:
    std::vector<PriceLevel> bidLevels;
    std::vector<PriceLevel> askLevels;
    std::unordered_map<OrderId, OrderRef> index;

    int bestBidIdx = -1;
    int bestAskIdx = NUM_LEVELS;

    static inline int toIndex(Price p) {
        return p - MIN_TICK;
    }

    static inline Price fromIndex(int idx) {
        return idx + MIN_TICK;
    }

    inline void updateBestBid() {
        while (bestBidIdx >= 0 && bidLevels[bestBidIdx].fifo.empty())
            --bestBidIdx;
    }

    inline void updateBestAsk() {
        while (bestAskIdx < NUM_LEVELS && askLevels[bestAskIdx].fifo.empty())
            ++bestAskIdx;
    }

    inline int bestBidBound() const {
        return NUM_LEVELS - 1;
    }

    inline int bestAskBound() const {
        return 0;
    }
};