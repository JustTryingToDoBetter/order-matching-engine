#pragma once

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

enum class Side { buy, sell };

using OrderId = int;

using Price = int;


struct Order {
    OrderId id;
    Side side;
    Price price;
    int qty;
};

struct TradeSink {
    long long tradeCount = 0;
    long long totalQty = 0;

    void onTrade(int qty, Price price, OrderId taker, OrderId maker) {
        (void)price; (void)taker; (void)maker;
        tradeCount++;
        totalQty += qty;
    }
};

struct PriceLevel {
    std::list<Order> fifo;  // FIFO at this price
    int totalQuantity = 0;
};

using Asks = std::map<Price, PriceLevel>;                       // low -> high
using Bids = std::map<double, PriceLevel, std::greater<double>>; // high -> low
using OrderIt = std::list<Order>::iterator;

struct OrderRef {
    Side side;
    Price price;
    OrderIt it;
};

// Add an order to the book AND index it by id.
// Returns false if duplicate id or qty invalid.
inline bool addToBook(Asks& asks,
                      Bids& bids,
                      std::unordered_map<OrderId, OrderRef>& index,
                      const Order& o) {
    if (o.qty <= 0) return false;
    if (index.find(o.id) != index.end()) return false; // reject duplicates

    if (o.side == Side::sell) {
        PriceLevel& level = asks[o.price];
        level.fifo.push_back(o);
        level.totalQuantity += o.qty;

        auto it = std::prev(level.fifo.end());
        index.emplace(o.id, OrderRef{o.side, o.price, it});
    } else {
        PriceLevel& level = bids[o.price];
        level.fifo.push_back(o);
        level.totalQuantity += o.qty;

        auto it = std::prev(level.fifo.end());
        index.emplace(o.id, OrderRef{o.side, o.price, it});
    }
    return true;
}

// Cancel by id (O(1)).
inline bool cancelOrder(Asks& asks,
                        Bids& bids,
                        std::unordered_map<OrderId, OrderRef>& index,
                        OrderId id) {
    auto itRef = index.find(id);
    if (itRef == index.end()) return false;

    const Side side = itRef->second.side;
    const double price = itRef->second.price;
    OrderIt orderIt = itRef->second.it;

    auto cancelFromLevel = [&](auto& book) -> bool {
        auto itLevel = book.find(price);
        if (itLevel == book.end()) return false;

        PriceLevel& level = itLevel->second;

        // Safety: iterator must still point to this id
        if (orderIt == level.fifo.end() || orderIt->id != id) return false;

        level.totalQuantity -= orderIt->qty;
        level.fifo.erase(orderIt);

        if (level.fifo.empty()) {
            book.erase(itLevel);
        }

        index.erase(itRef);
        return true;
    };

    return (side == Side::sell) ? cancelFromLevel(asks) : cancelFromLevel(bids);
}

// Matching. No printing. Emits fills into sink.
inline void matchIncoming(Asks& asks,
                          Bids& bids,
                          std::unordered_map<OrderId, OrderRef>& index,
                          Order incoming,
                          TradeSink& sink) {
    if (incoming.qty <= 0) return;

    if (incoming.side == Side::buy) {
        while (incoming.qty > 0 && !asks.empty()) {
            auto bestAskIt = asks.begin();
            const Price bestAskPrice = bestAskIt->first;

            if (bestAskPrice > incoming.price) break;

            PriceLevel& level = bestAskIt->second;

            while (incoming.qty > 0 && !level.fifo.empty()) {
                auto makerIt = level.fifo.begin();
                Order& maker = *makerIt;

                const int fill = std::min(incoming.qty, maker.qty);

                maker.qty -= fill;
                incoming.qty -= fill;
                level.totalQuantity -= fill;

                sink.onTrade(fill, bestAskPrice, incoming.id, maker.id);

                if (maker.qty == 0) {
                    index.erase(maker.id);
                    level.fifo.erase(makerIt);
                }
            }

            if (level.fifo.empty()) {
                asks.erase(bestAskIt);
            }
        }

        if (incoming.qty > 0) {
            (void)addToBook(asks, bids, index, incoming);
        }
    } else {
        while (incoming.qty > 0 && !bids.empty()) {
            auto bestBidIt = bids.begin();
            const Price bestBidPrice = bestBidIt->first;

            if (bestBidPrice < incoming.price) break;

            PriceLevel& level = bestBidIt->second;

            while (incoming.qty > 0 && !level.fifo.empty()) {
                auto makerIt = level.fifo.begin();
                Order& maker = *makerIt;

                const int fill = std::min(incoming.qty, maker.qty);

                maker.qty -= fill;
                incoming.qty -= fill;
                level.totalQuantity -= fill;

                sink.onTrade(fill, bestBidPrice, incoming.id, maker.id);

                if (maker.qty == 0) {
                    index.erase(maker.id);
                    level.fifo.erase(makerIt);
                }
            }

            if (level.fifo.empty()) {
                bids.erase(bestBidIt);
            }
        }

        if (incoming.qty > 0) {
            (void)addToBook(asks, bids, index, incoming);
        }
    }
}

// Replace: cancel old and treat new order as incoming (may execute).
inline bool replaceOrder(Asks& asks,
                         Bids& bids,
                         std::unordered_map<OrderId, OrderRef>& index,
                         OrderId id,
                         Price newPrice,
                         int newQty,
                         TradeSink& sink) {
    auto itRef = index.find(id);
    if (itRef == index.end()) return false;

    const Side side = itRef->second.side;

    if (!cancelOrder(asks, bids, index, id)) return false;

    matchIncoming(asks, bids, index, Order{id, side, newPrice, newQty}, sink);
    return true;
}