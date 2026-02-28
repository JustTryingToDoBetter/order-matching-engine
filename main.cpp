#include  <iostream>
#include <map> 
#include <functional>
using namespace std;

enum class Side {
    buy, 
    sell
};

struct Order {int id; Side side; int price; int qty;};


struct priceLevel {int totalQuantity = 0;};

int main() {

    std::map<double, priceLevel> asks;
    std::map<double, priceLevel, std::greater<double>> bids;


    asks[100.5].totalQuantity = 5;
    asks[100.5].totalQuantity += 2; 
    bids[101.2].totalQuantity  = 10;
    bids[101.2].totalQuantity += 4;


    //best ask
    if (!asks.empty()){
        auto bestAsk = asks.begin();
        double price = bestAsk->first;
        int totalQty = bestAsk->second.totalQuantity;

        std ::cout << "Best Ask: " <<price << "\n";
        std ::cout << "Total Quauntity: " <<totalQty << "\n";
    }
    else{ std :: cout << "No asks available";}

    //best bid
    if (!bids.empty()){
        auto bestBid = bids.begin();
        double bid = bestBid->first;
        int totalQty = bestBid->second.totalQuantity;

        std::cout << "Best Bid: " <<bid << "\n";
        std ::cout << "Total Quauntity: " <<totalQty << "\n";

    }
    else{std ::cout << "No bids available";}

}

