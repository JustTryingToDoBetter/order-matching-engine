#include "engine.hpp"

#include <iostream>
#include <random>
#include <chrono>
#include <vector>

// Generate a random price around 100.00 with +/- 50 ticks (0.01).
static double randPrice(std::mt19937& rng){
    std::uniform_int_distribution<int> ticks(-50, 50);
    return 100.0 + ticks(rng) * 0.01;
}

int main(){
    constexpr int N = 1'000'000; // number of orders to simulate
    Asks asks; // low -> high
    Bids bids; // high -> low
    std::unordered_map<OrderId, OrderRef> index; // id -> (side, price, iterator)
    index.reserve(N); // reserve for performance / stability in benchmarks

    TradeSink sink; // collect trade stats

    std::mt19937 rng(12345); // deterministic seed for reproducibility
    std::uniform_int_distribution<int> opDist(1, 10); // 1-7 for new order, 8-9 for cancel, 10 for replace
    std::uniform_int_distribution<int> sideDist(0, 1); // 0 for buy, 1 for sell
    std::uniform_int_distribution<int> qtyDist(1, 10); // order quantity between 1 and 10

    int nextId = 1; // incremental order IDs
    std::vector<int> liveIDs;// track live order IDs for cancel/replace
    liveIDs.reserve(N);// track live order IDs for cancel/replace


    auto t0 = std::chrono::steady_clock::now(); // start timer
    
    for (int i =0; i< N; i++){
        int r = opDist(rng);
        if (r < 60){
            Side side = sideDist(rng) == 0 ? Side::buy : Side::sell; // random side
            double price = randPrice(rng); // random price around 100.00
            int qty = qtyDist(rng); // random quantity
            int id = nextId++; // assign unique ID

            matchIncoming(asks, bids, index, Order{id, side, price, qty}, sink);

            if (index.find(id) != index.end()) liveIDs.push_back(id); // track live order ID   
            
        } else if (r < 85){
            // Cancel a random live order
            if (!liveIDs.empty()){
                std::uniform_int_distribution<size_t> pick(0, liveIDs.size() - 1); // pick random index
                size_t pos = pick(rng); // get random position
                int id = liveIDs[pos]; // get order ID to cancel

                //swap pop on success or stale
                (void)cancelOrder(asks, bids, index, id); // cancel order, ignore result (could be already filled/cancelled)
                liveIDs[pos] = liveIDs.back(); // swap with last
                liveIDs.pop_back(); // remove last
            }
        }else {
            if (!liveIDs.empty()){
                std::uniform_int_distribution<size_t> pick(0, liveIDs.size() - 1); // pick random index

                int id = liveIDs[pick(rng)]; // get order ID to replace

                double newPrice = randPrice(rng); // new random price
                int newQty = qtyDist(rng); // new random quantity

                (void) replaceOrder(asks,bids, index, id, newPrice, newQty, sink); // replace order, ignore result (could be already filled/cancelled   )
        }
    }

    auto t1 = std::chrono::steady_clock::now(); // end timer
    std::chrono::duration<double> dt = t1 - t0;

    std::cout << "Ops: " << N << "\n";
    std::cout << "Seconds: " << dt.count() << "\n";
    std::cout << "Ops/sec: " << (N / dt.count()) << "\n";
    std::cout << "Trades: " << sink.tradeCount << "\n";
    std::cout << "Total filled qty: " << sink.totalQty << "\n";
    std::cout << "Live orders: " << index.size() << "\n";
}

}