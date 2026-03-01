#include <algorithm>   // std::min
#include <functional>  // std::greater
#include <iostream>
#include <list>
#include <map>
#include <unordered_map>

enum class Side { buy, sell };

using OrderId = int;

// NOTE: still using double for now because you chose it.
// We'll migrate to integer ticks later.
struct Order {
    OrderId id;
    Side side;
    double price;
    int qty;
};

struct PriceLevel {
    std::list<Order> fifo;   // FIFO queue at this price
    int totalQuantity = 0;   // sum of remaining qty at this price
};

using Asks = std::map<double, PriceLevel>;                       // low -> high
using Bids = std::map<double, PriceLevel, std::greater<double>>; // high -> low

using OrderIt = std::list<Order>::iterator;

// We track side + price + iterator for O(1) cancel/replace.
struct OrderRef {
    Side side;
    double price;
    OrderIt it; // points to the live order inside its price level FIFO
};

// Forward declare (needed because replaceOrder calls matchIncoming).
void matchIncoming(Asks& asks,
                   Bids& bids,
                   std::unordered_map<OrderId, OrderRef>& index,
                   Order incoming);

// Add an order to the book AND index it by id.
// Returns false if duplicate id.
bool addToBook(Asks& asks,
               Bids& bids,
               std::unordered_map<OrderId, OrderRef>& index,
               const Order& o) {
    if (o.qty <= 0) return false;

    // Portfolio-grade: reject duplicate IDs.
    if (index.find(o.id) != index.end()) return false;

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

// Cancel by id (O(1) remove using stored iterator).
// Returns true if cancelled, false if id not found.
bool cancelOrder(Asks& asks,
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

        // Safety check: if iterator points to wrong order, something broke.
        // (Should never happen if we reject duplicate IDs and update index correctly.)
        if (orderIt == level.fifo.end() || orderIt->id != id) return false;

        level.totalQuantity -= orderIt->qty;
        level.fifo.erase(orderIt);

        if (level.fifo.empty()) {
            book.erase(itLevel);
        }

        index.erase(itRef);
        return true;
    };

    if (side == Side::sell) return cancelFromLevel(asks);
    return cancelFromLevel(bids);
}

// Replace: cancel old and treat new order as an incoming order (may execute immediately).
bool replaceOrder(Asks& asks,
                  Bids& bids,
                  std::unordered_map<OrderId, OrderRef>& index,
                  OrderId id,
                  double newPrice,
                  int newQty) {
    auto itRef = index.find(id);
    if (itRef == index.end()) return false;

    const Side side = itRef->second.side;

    if (!cancelOrder(asks, bids, index, id)) return false;

    // Important: do NOT addToBook() first; matchIncoming will add remainder exactly once.
    matchIncoming(asks, bids, index, Order{id, side, newPrice, newQty});
    return true;
}

// Matching. Emits trades to stdout for now.
void matchIncoming(Asks& asks,
                   Bids& bids,
                   std::unordered_map<OrderId, OrderRef>& index,
                   Order incoming) {
    if (incoming.qty <= 0) return;

    if (incoming.side == Side::buy) {
        while (incoming.qty > 0 && !asks.empty()) {
            auto bestAskIt = asks.begin();
            const double bestAskPrice = bestAskIt->first;

            if (bestAskPrice > incoming.price) break;

            PriceLevel& level = bestAskIt->second;

            // Match against FIFO at this price
            while (incoming.qty > 0 && !level.fifo.empty()) {
                auto makerIt = level.fifo.begin();
                Order& maker = *makerIt;

                const int fill = std::min(incoming.qty, maker.qty);

                maker.qty -= fill;
                incoming.qty -= fill;
                level.totalQuantity -= fill;

                std::cout << "Trade: qty=" << fill << " @ " << bestAskPrice
                          << " (buy " << incoming.id << " vs sell " << maker.id << ")\n";

                if (maker.qty == 0) {
                    // Maker fully filled -> remove from index + FIFO
                    index.erase(maker.id);
                    level.fifo.erase(makerIt);
                } else {
                    // Maker still resting at front; stop matching this level only if incoming is done.
                    // Otherwise continue (incoming may still have qty).
                }
            }

            if (level.fifo.empty()) {
                asks.erase(bestAskIt);
            }
        }

        if (incoming.qty > 0) {
            // Remainder becomes resting
            if (!addToBook(asks, bids, index, incoming)) {
                // Duplicate ID or invalid qty; in a real engine you'd reject upstream.
                // For portfolio: fail loudly in debug builds.
                std::cerr << "ERROR: failed to add remainder for id=" << incoming.id << "\n";
            }
        }
    } else {
        while (incoming.qty > 0 && !bids.empty()) {
            auto bestBidIt = bids.begin();
            const double bestBidPrice = bestBidIt->first;

            if (bestBidPrice < incoming.price) break;

            PriceLevel& level = bestBidIt->second;

            while (incoming.qty > 0 && !level.fifo.empty()) {
                auto makerIt = level.fifo.begin();
                Order& maker = *makerIt;

                const int fill = std::min(incoming.qty, maker.qty);

                maker.qty -= fill;
                incoming.qty -= fill;
                level.totalQuantity -= fill;

                std::cout << "Trade: qty=" << fill << " @ " << bestBidPrice
                          << " (sell " << incoming.id << " vs buy " << maker.id << ")\n";

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
            if (!addToBook(asks, bids, index, incoming)) {
                std::cerr << "ERROR: failed to add remainder for id=" << incoming.id << "\n";
            }
        }
    }
}

void printTop(const Asks& asks, const Bids& bids) {
    if (!asks.empty()) {
        auto it = asks.begin();
        std::cout << "Best Ask: " << it->first
                  << " (qty " << it->second.totalQuantity << ")";
        if (!it->second.fifo.empty())
            std::cout << " | head id=" << it->second.fifo.front().id
                      << " head qty=" << it->second.fifo.front().qty;
        std::cout << "\n";
    } else {
        std::cout << "Best Ask: N/A\n";
    }

    if (!bids.empty()) {
        auto it = bids.begin();
        std::cout << "Best Bid: " << it->first
                  << " (qty " << it->second.totalQuantity << ")";
        if (!it->second.fifo.empty())
            std::cout << " | head id=" << it->second.fifo.front().id
                      << " head qty=" << it->second.fifo.front().qty;
        std::cout << "\n";
    } else {
        std::cout << "Best Bid: N/A\n";
    }
}

int main() {
    Asks asks;
    Bids bids;
    std::unordered_map<OrderId, OrderRef> index;

    // Good habit: reserve for performance / stability in benchmarks.
    index.reserve(1024);

    // Seed book
    addToBook(asks, bids, index, Order{1, Side::sell, 100.5, 5});
    addToBook(asks, bids, index, Order{2, Side::sell, 100.5, 2});
    addToBook(asks, bids, index, Order{3, Side::buy,  99.8,  4});
    addToBook(asks, bids, index, Order{4, Side::buy,  99.9,  6});

    std::cout << "=== Before replace/cancel ===\n";
    printTop(asks, bids);

    std::cout << "\n=== Replace BUY id=4 -> new price 101.0 qty 6 ===\n";
    std::cout << (replaceOrder(asks, bids, index, 4, 101.0, 6) ? "REPLACED\n" : "REPLACE FAILED\n");
    printTop(asks, bids);

    std::cout << "\nCancel order id=2...\n";
    std::cout << (cancelOrder(asks, bids, index, 2) ? "CANCELLED\n" : "NOT FOUND\n");
    printTop(asks, bids);

    std::cout << "\n=== Incoming BUY id=10 @101.0 qty=6 ===\n";
    matchIncoming(asks, bids, index, Order{10, Side::buy, 101.0, 6});
    printTop(asks, bids);

    std::cout << "\nCancel order id=1...\n";
    std::cout << (cancelOrder(asks, bids, index, 1) ? "CANCELLED\n" : "NOT FOUND\n");
    printTop(asks, bids);

    return 0;
}