#include <iostream>
#include <map>
#include <deque>
#include <functional>  // std::greater
#include <algorithm>   // std::min

// Keep your existing enum names for consistency
enum class Side { buy, sell };

struct Order {
    int id;
    Side side;
    double price;  // Note: doubles are OK for a toy; real systems use integer ticks
    int qty;
};

// Each price level: FIFO of maker orders + running total for fast top-of-book
struct PriceLevel {
    std::deque<Order> fifo;  // FIFO queue at this price
    int totalQuantity = 0;   // sum of remaining qty at this price
};

// Order books
using Asks = std::map<double, PriceLevel>;                           // lowest price first
using Bids = std::map<double, PriceLevel, std::greater<double>>;     // highest price first

// Push an order into the appropriate book (does NOT try to match)
// - appends to FIFO
// - increments totalQuantity
void addToBook(Asks& asks, Bids& bids, const Order& o) {
    if (o.side == Side::sell) {
        PriceLevel& level = asks[o.price];   // creates level if absent
        level.fifo.push_back(o);             // FIFO: new order to the back
        level.totalQuantity += o.qty;
    } else {
        PriceLevel& level = bids[o.price];
        level.fifo.push_back(o);
        level.totalQuantity += o.qty;
    }
}

// Try to match an incoming order against the opposite book (FIFO within price)
// - Buy matches vs asks while best ask price <= incoming.price
// - Sell matches vs bids while best bid price >= incoming.price
// - Generates "trades" (just std::cout here)
// - Any remaining qty is added to the appropriate book
void matchIncoming(Asks& asks, Bids& bids, Order incoming) {
    if (incoming.qty <= 0) return;

    if (incoming.side == Side::buy) {
        // Match vs best asks
        while (incoming.qty > 0 && !asks.empty()) {
            auto bestAskIt = asks.begin();          // lowest ask
            double bestAskPrice = bestAskIt->first; // price key

            if (bestAskPrice > incoming.price) break; // no more marketable asks

            PriceLevel& level = bestAskIt->second;
            Order& maker = level.fifo.front();      // FIFO head at that price

            int fill = std::min(incoming.qty, maker.qty);
            maker.qty -= fill;
            level.totalQuantity -= fill;
            incoming.qty -= fill;

            std::cout << "Trade: qty=" << fill
                      << " @ " << bestAskPrice
                      << " (buy " << incoming.id
                      << " vs sell " << maker.id << ")\n";

            if (maker.qty == 0) {
                level.fifo.pop_front();
            }
            if (level.fifo.empty() || level.totalQuantity == 0) {
                asks.erase(bestAskIt);  // price level empty -> remove
            }
        }

        // If not fully matched, rest becomes a bid
        if (incoming.qty > 0) {
            addToBook(asks, bids, incoming);
        }
    } else {
        // Sell: match vs best bids
        while (incoming.qty > 0 && !bids.empty()) {
            auto bestBidIt = bids.begin();          // highest bid
            double bestBidPrice = bestBidIt->first;

            if (bestBidPrice < incoming.price) break; // no more marketable bids

            PriceLevel& level = bestBidIt->second;
            Order& maker = level.fifo.front();

            int fill = std::min(incoming.qty, maker.qty);
            maker.qty -= fill;
            level.totalQuantity -= fill;
            incoming.qty -= fill;

            std::cout << "Trade: qty=" << fill
                      << " @ " << bestBidPrice
                      << " (sell " << incoming.id
                      << " vs buy " << maker.id << ")\n";

            if (maker.qty == 0) {
                level.fifo.pop_front();
            }
            if (level.fifo.empty() || level.totalQuantity == 0) {
                bids.erase(bestBidIt);
            }
        }

        // If not fully matched, rest becomes an ask
        if (incoming.qty > 0) {
            addToBook(asks, bids, incoming);
        }
    }
}

// Helpers to print top of book (for debugging)
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

    // Seed book with a couple of makers
    addToBook(asks, bids, Order{1, Side::sell, 100.5, 5}); // sell @100.5
    addToBook(asks, bids, Order{2, Side::sell, 100.5, 2}); // same price (FIFO grows)
    addToBook(asks, bids, Order{3, Side::buy,  99.8,  4}); // buy @99.8
    addToBook(asks, bids, Order{4, Side::buy,  99.9,  6}); // buy @99.9 (best bid)

    std::cout << "=== Before incoming ===\n";
    printTop(asks, bids);

    // Incoming BUY that should match asks up to price 101.0
    std::cout << "\n=== Incoming BUY id=10 @101.0 qty=6 ===\n";
    matchIncoming(asks, bids, Order{10, Side::buy, 101.0, 6});
    printTop(asks, bids);

    // Incoming SELL that should match bids down to price 99.8
    std::cout << "\n=== Incoming SELL id=11 @99.8 qty=5 ===\n";
    matchIncoming(asks, bids, Order{11, Side::sell, 99.8, 5});
    printTop(asks, bids);

    return 0;
}