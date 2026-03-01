#include <algorithm>   // std::min
#include <deque>
#include <functional>  // std::greater
#include <iostream>
#include <map>
#include <optional>
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
    std::deque<Order> fifo;
    int totalQuantity = 0;
};

using Asks = std::map<double, PriceLevel>;                       // low -> high
using Bids = std::map<double, PriceLevel, std::greater<double>>; // high -> low

/// --- We need to track side + price for cancellations. This is our "index" value.
struct OrderRef {
    Side side;
    double price;
};

// --- Add an order to the book AND index it by id.
void addToBook(Asks& asks,
               Bids& bids,
               std::unordered_map<OrderId, OrderRef>& index,
               const Order& o) {
    // If you want strictness: reject duplicate IDs.
    // For now, we’ll overwrite (not ideal). We'll harden later.
    index[o.id] = OrderRef{o.side, o.price};

    if (o.side == Side::sell) {
        PriceLevel& level = asks[o.price];
        level.fifo.push_back(o);
        level.totalQuantity += o.qty;
    } else {
        PriceLevel& level = bids[o.price];
        level.fifo.push_back(o);
        level.totalQuantity += o.qty;
    }
}

// --- Cancel by id (v1: scan deque at that price).
// Returns true if cancelled, false if id not found.
bool cancelOrder(Asks& asks,
                 Bids& bids,
                 std::unordered_map<OrderId, OrderRef>& index,
                 OrderId id) {
    auto itRef = index.find(id);
    if (itRef == index.end()) return false;

    const Side side = itRef->second.side;
    const double price = itRef->second.price;

    auto cancelFromLevel = [&](auto& book) -> bool {
        auto itLevel = book.find(price);
        if (itLevel == book.end()) return false;

        PriceLevel& level = itLevel->second;

        // Find the order in FIFO
        for (auto it = level.fifo.begin(); it != level.fifo.end(); ++it) {
            if (it->id == id) {
                // Update totals first
                level.totalQuantity -= it->qty;

                // Remove order
                level.fifo.erase(it);

                // Clean up empty price levels
                if (level.fifo.empty()) {
                    book.erase(itLevel);
                }

                // Remove from index
                index.erase(itRef);
                return true;
            }
        }
        return false;
    };

    if (side == Side::sell) return cancelFromLevel(asks);
    return cancelFromLevel(bids);
}


bool replaceOrder(Asks& asks,
                  Bids& bids,
                  std::unordered_map<OrderId, OrderRef>& index, 
                  OrderId id,
                  double newPrice,
                  int newQty) {
    auto itRef = index.find(id); // Find the order in the index
    if (itRef == index.end()) return false; // ID not found

    const Side side = itRef->second.side; // Get the side and price from the index
    const double oldPrice = itRef->second.price; // We might need this if we want to optimize the cancel step later

    // Cancel the existing order
    if (!cancelOrder(asks, bids, index, id)) {
        return false; // Should not happen since we found the ID in the index
    }

    // Add the new order with the same ID but updated price and quantity
    addToBook(asks, bids, index, Order{id, side, newPrice, newQty});
    // matchIncoming can also be used here instead of addToBook if we want the replace to potentially execute immediately against the book. For now, we'll keep it simple and just add to book.
    matchIncoming(asks, bids, index, Order{id, side, newPrice, newQty});
    return true;
}



// --- Matching (kept simple). Emits trades to stdout for now.
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
            Order& maker = level.fifo.front();

            const int fill = std::min(incoming.qty, maker.qty);

            maker.qty -= fill;
            incoming.qty -= fill;
            level.totalQuantity -= fill;

            std::cout << "Trade: qty=" << fill << " @ " << bestAskPrice
                      << " (buy " << incoming.id << " vs sell " << maker.id << ")\n";

            if (maker.qty == 0) {
                // Maker fully filled -> remove from index + FIFO
                index.erase(maker.id);
                level.fifo.pop_front();
            }

            if (level.fifo.empty()) {
                asks.erase(bestAskIt);
            }
        }

        if (incoming.qty > 0) {
            addToBook(asks, bids, index, incoming);
        }
    } else {
        while (incoming.qty > 0 && !bids.empty()) {
            auto bestBidIt = bids.begin();
            const double bestBidPrice = bestBidIt->first;

            if (bestBidPrice < incoming.price) break;

            PriceLevel& level = bestBidIt->second;
            Order& maker = level.fifo.front();

            const int fill = std::min(incoming.qty, maker.qty);

            maker.qty -= fill;
            incoming.qty -= fill;
            level.totalQuantity -= fill;

            std::cout << "Trade: qty=" << fill << " @ " << bestBidPrice
                      << " (sell " << incoming.id << " vs buy " << maker.id << ")\n";

            if (maker.qty == 0) {
                index.erase(maker.id);
                level.fifo.pop_front();
            }

            if (level.fifo.empty()) {
                bids.erase(bestBidIt);
            }
        }

        if (incoming.qty > 0) {
            addToBook(asks, bids, index, incoming);
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


static int nextOrderId = 100; // For generating new order IDs in tests if needed.
static int newOrderId() { return nextOrderId++; }

int main() {
    Asks asks;
    Bids bids;
    std::unordered_map<OrderId, OrderRef> index;

    // Seed book
    addToBook(asks, bids, index, Order{1, Side::sell, 100.5, 5});
    addToBook(asks, bids, index, Order{2, Side::sell, 100.5, 2});
    addToBook(asks, bids, index, Order{3, Side::buy,  99.8,  4});
    addToBook(asks, bids, index, Order{4, Side::buy,  99.9,  6});

    std::cout << "=== Before cancel ===\n";
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