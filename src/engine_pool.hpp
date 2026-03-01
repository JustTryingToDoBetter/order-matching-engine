#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

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

// ---------------- Intrusive node ----------------

struct OrderNode {
    Order o{};
    OrderNode* prev = nullptr;
    OrderNode* next = nullptr;
};

// ---------------- Pool allocator (free-list) ----------------

class NodePool {
public:
    explicit NodePool(std::size_t reserve_nodes = 0) {
        if (reserve_nodes) reserve(reserve_nodes);
    }

    void reserve(std::size_t n) {
        // Reserve backing storage to reduce reallocations.
        // Nodes are stable because we store them in a vector and never move them after creation:
        // We must ensure vector doesn't reallocate after pointers are handed out.
        // So: reserve once up front, or grow in chunks (we do chunk grow).
        storage.reserve(storage.size() + n);
        // Pre-create nodes and push to free list
        for (std::size_t i = 0; i < n; ++i) {
            storage.emplace_back();
            free_list.push_back(&storage.back());
        }
    }

    OrderNode* alloc(const Order& o) {
        if (free_list.empty()) {
            // Grow in chunks to amortize.
            reserve(grow_chunk);
        }
        OrderNode* n = free_list.back();
        free_list.pop_back();

        n->o = o;
        n->prev = nullptr;
        n->next = nullptr;
        return n;
    }

    void free(OrderNode* n) {
        // Reset is optional; we keep it cheap.
        n->prev = nullptr;
        n->next = nullptr;
        free_list.push_back(n);
    }

    std::size_t freeCount() const { return free_list.size(); }
    std::size_t capacity()  const { return storage.size(); }

private:
    // NOTE: vector reallocation would invalidate pointers, so we only grow by reserve+emplace_back
    // and ensure reserve is called before pushing nodes. This implementation uses storage.emplace_back
    // and takes &storage.back(); this is safe only if no reallocation happens during the loop.
    // reserve() ensures enough capacity for the additional nodes.
    std::vector<OrderNode> storage;
    std::vector<OrderNode*> free_list;

    static constexpr std::size_t grow_chunk = 1 << 16; // 65,536 nodes per grow
};

// ---------------- Price Level ----------------

struct PriceLevel {
    OrderNode* head = nullptr;
    OrderNode* tail = nullptr;
    int totalQuantity = 0;

    inline bool empty() const { return head == nullptr; }

    inline void push_back(OrderNode* n) {
        n->prev = tail;
        n->next = nullptr;

        if (tail) tail->next = n;
        else head = n;

        tail = n;
    }

    inline void pop_front() {
        OrderNode* n = head;
        if (!n) return;

        OrderNode* nxt = n->next;
        head = nxt;
        if (nxt) nxt->prev = nullptr;
        else tail = nullptr;

        n->prev = n->next = nullptr;
    }

    inline void erase(OrderNode* n) {
        OrderNode* p = n->prev;
        OrderNode* q = n->next;

        if (p) p->next = q;
        else head = q;

        if (q) q->prev = p;
        else tail = p;

        n->prev = n->next = nullptr;
    }
};

// ---------------- Engine ----------------

// Bounded tick band for benchmark.
constexpr Price MIN_TICK = 900;
constexpr Price MAX_TICK = 1100;
constexpr int   NUM_LEVELS = MAX_TICK - MIN_TICK + 1;

struct OrderRef {
    Side side;
    Price price;
    OrderNode* node;
};

class OrderBookPool {
public:
    explicit OrderBookPool(std::size_t expected_orders = 0)
        : pool(expected_orders) {
        bidLevels.resize(NUM_LEVELS);
        askLevels.resize(NUM_LEVELS);
        index.reserve(expected_orders ? expected_orders : 1024);
        index.max_load_factor(0.7f);
    }

    inline void reserve(std::size_t expected_orders) {
        pool.reserve(expected_orders);
        index.reserve(expected_orders);
    }

    inline void matchIncoming(const Order& incoming, TradeSink& sink) {
        Order in = incoming;
        if (in.qty <= 0) return;
        if (in.price < MIN_TICK || in.price > MAX_TICK) return;

        if (in.side == Side::buy) {
            // Match against best asks while crossed
            while (in.qty > 0 && bestAskIdx < NUM_LEVELS) {
                if (bestAskIdx > toIndex(in.price)) break;

                PriceLevel& level = askLevels[bestAskIdx];

                while (in.qty > 0 && level.head) {
                    OrderNode* makerNode = level.head;
                    Order& maker = makerNode->o;

                    int fill = std::min(in.qty, maker.qty);

                    maker.qty -= fill;
                    in.qty    -= fill;
                    level.totalQuantity -= fill;

                    sink.onTrade(fill, fromIndex(bestAskIdx), in.id, maker.id);

                    if (maker.qty == 0) {
                        // Remove maker from index and level
                        index.erase(maker.id);
                        level.pop_front();
                        pool.free(makerNode);
                    } else {
                        // maker still at front
                        break;
                    }
                }

                if (!level.head) {
                    updateBestAsk();
                } else {
                    break;
                }
            }

            if (in.qty > 0) {
                (void)addToBook(in);
            }
        } else {
            while (in.qty > 0 && bestBidIdx >= 0) {
                if (bestBidIdx < toIndex(in.price)) break;

                PriceLevel& level = bidLevels[bestBidIdx];

                while (in.qty > 0 && level.head) {
                    OrderNode* makerNode = level.head;
                    Order& maker = makerNode->o;

                    int fill = std::min(in.qty, maker.qty);

                    maker.qty -= fill;
                    in.qty    -= fill;
                    level.totalQuantity -= fill;

                    sink.onTrade(fill, fromIndex(bestBidIdx), in.id, maker.id);

                    if (maker.qty == 0) {
                        index.erase(maker.id);
                        level.pop_front();
                        pool.free(makerNode);
                    } else {
                        break;
                    }
                }

                if (!level.head) {
                    updateBestBid();
                } else {
                    break;
                }
            }

            if (in.qty > 0) {
                (void)addToBook(in);
            }
        }
    }

    inline bool cancel(OrderId id) {
        auto it = index.find(id);
        if (it == index.end()) return false;

        OrderNode* n = it->second.node;
        const Side side = it->second.side;
        const Price price = it->second.price;

        int idx = toIndex(price);
        PriceLevel& level = (side == Side::buy) ? bidLevels[idx] : askLevels[idx];

        level.totalQuantity -= n->o.qty;
        level.erase(n);

        index.erase(it);
        pool.free(n);

        if (level.empty()) {
            if (side == Side::buy && idx == bestBidIdx) updateBestBid();
            if (side == Side::sell && idx == bestAskIdx) updateBestAsk();
        }

        return true;
    }

    inline bool replace(OrderId id, Price newPrice, int newQty, TradeSink& sink) {
        auto it = index.find(id);
        if (it == index.end()) return false;

        Side side = it->second.side;
        if (!cancel(id)) return false;

        matchIncoming(Order{id, side, newPrice, newQty}, sink);
        return true;
    }

    inline std::size_t liveOrders() const { return index.size(); }

private:
    std::vector<PriceLevel> bidLevels;
    std::vector<PriceLevel> askLevels;

    std::unordered_map<OrderId, OrderRef> index;
    NodePool pool;

    int bestBidIdx = -1;
    int bestAskIdx = NUM_LEVELS;

    static inline int toIndex(Price p) { return p - MIN_TICK; }
    static inline Price fromIndex(int idx) { return idx + MIN_TICK; }

    inline bool addToBook(const Order& o) {
        if (o.qty <= 0) return false;
        if (o.price < MIN_TICK || o.price > MAX_TICK) return false;
        if (index.find(o.id) != index.end()) return false;

        OrderNode* n = pool.alloc(o);
        int idx = toIndex(o.price);

        if (o.side == Side::buy) {
            PriceLevel& level = bidLevels[idx];
            level.push_back(n);
            level.totalQuantity += o.qty;

            index.emplace(o.id, OrderRef{o.side, o.price, n});
            if (bestBidIdx < idx) bestBidIdx = idx;
        } else {
            PriceLevel& level = askLevels[idx];
            level.push_back(n);
            level.totalQuantity += o.qty;

            index.emplace(o.id, OrderRef{o.side, o.price, n});
            if (bestAskIdx > idx) bestAskIdx = idx;
        }
        return true;
    }

    inline void updateBestBid() {
        while (bestBidIdx >= 0 && bidLevels[bestBidIdx].empty())
            --bestBidIdx;
    }

    inline void updateBestAsk() {
        while (bestAskIdx < NUM_LEVELS && askLevels[bestAskIdx].empty())
            ++bestAskIdx;
    }
};