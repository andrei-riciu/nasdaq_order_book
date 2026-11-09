#include "itch/book.hpp"
#include "itch/messages.hpp"

#include <algorithm>
#include <cassert>

namespace itch {

OrderBook::OrderBook() : books_(65536), symbols_(65536) {}

Levels &OrderBook::levels(std::uint16_t locate, char side) {
  Book &b = books_[locate];
  return side == 'B' ? b.bids : b.asks;
}

void OrderBook::add(std::uint64_t ref, std::uint16_t locate, char side,
                    std::uint32_t shares, std::uint32_t price) {
  const auto [it, inserted] =
      orders_.try_emplace(ref, Order{price, shares, locate, side});
  if (!inserted) {
    ++stats_.duplicate_refs; // keep the original; don't double-count shares
    return;
  }
  Level &lvl = levels(locate, side)[price]; // creates an empty level if new
  lvl.shares += shares;
  ++lvl.orders;
  stats_.peak_orders = std::max(stats_.peak_orders, orders_.size());
}

void OrderBook::reduce(std::uint64_t ref, std::uint32_t shares) {
  const auto it = orders_.find(ref);
  if (it == orders_.end()) {
    ++stats_.missing_refs;
    return;
  }
  Order &o = it->second;
  if (shares > o.shares) {
    ++stats_.overfills;
    shares = o.shares; // never let a level go negative
  }

  Levels &side = levels(o.locate, o.side);
  const auto lvl = side.find(o.price);
  assert(lvl != side.end() && "order exists but its level does not");
  lvl->second.shares -= shares;
  o.shares -= shares;

  if (o.shares == 0) {
    if (--lvl->second.orders == 0) {
      assert(lvl->second.shares == 0);
      side.erase(lvl); // no empty levels, so begin()/rbegin() is always real
    }
    orders_.erase(it); // last: `o` refers into the map
  }
}

void OrderBook::on_stock_directory(const StockDirectory &m) {
  symbols_[m.hdr.locate] = m.stock;
}

void OrderBook::on_add(const AddOrder &m) {
  add(m.ref, m.hdr.locate, m.side, m.shares, m.price);
}

void OrderBook::on_order_executed(const OrderExecuted &m) {
  reduce(m.ref, m.shares);
}

void OrderBook::on_order_executed_with_price(const OrderExecutedWithPrice &m) {
  reduce(m.ref, m.shares);
}

void OrderBook::on_order_cancel(const OrderCancel &m) {
  reduce(m.ref, m.shares);
}

void OrderBook::on_order_delete(const OrderDelete &m) {
  const auto it = orders_.find(m.ref);
  if (it == orders_.end()) {
    ++stats_.missing_refs;
    return;
  }
  Order &o = (orders_.find(m.ref))->second;

  reduce(m.ref, o.shares);
}
} // namespace itch
