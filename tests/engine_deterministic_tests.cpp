#include <cstdlib>
#include <iostream>
#include <string>

#include "engine_pool.hpp"

namespace {

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

    expect(name, book.replace(40, 1001, 6, sink), "replace must succeed for live id");

    expect(name, sink.tradeCount == 1, "replace-generated incoming order must trade once");
    expect(name, sink.totalQty == 4, "replace-generated incoming order must trade expected qty");
    expect(name, book.liveOrders() == 1, "replace should remove old order, trade, and rest remainder");

    expect(name, book.cancel(40), "id must point to newly inserted live remainder");
    expect(name, !book.cancel(40), "second cancel must fail after removing replaced order");
    expect(name, !book.cancel(41), "maker fully filled by replace must no longer be live");

    std::cout << "PASS: " << name << "\n";
}

} // namespace

int main() {
    scenario_fully_crossing_incoming_does_not_rest();
    scenario_partial_fill_remainder_rests_and_is_indexed();
    scenario_cancel_remove_once_second_fails();
    scenario_replace_cancel_reinsert_and_index_points_to_new_live_slot();

    std::cout << "PASS: all deterministic engine scenarios\n";
    return 0;
}
