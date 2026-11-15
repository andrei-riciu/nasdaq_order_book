#include "itch/book.hpp"
#include "itch/messages.hpp"

#include <algorithm>
#include <cassert>
#include <optional>
#include <vector>

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
  Order &o = it->second;

  reduce(m.ref, o.shares);
}

void OrderBook::on_order_replace(const OrderReplace &m) {
  const auto it = orders_.find(m.old_ref);
  if (it == orders_.end()) {
    ++stats_.missing_refs;
    return;
  }
  std::uint16_t locate = it->second.locate;
  char side = it->second.side;

  reduce(m.old_ref, it->second.shares);
  add(m.new_ref, locate, side, m.shares, m.price);
}

std::optional<Quote> OrderBook::best_bid(std::uint16_t locate) const {
  const Levels &bids = books_[locate].bids; // reference: no copy of the map
  if (bids.empty())
    return std::nullopt;
  const auto it = bids.rbegin(); // highest price
  return Quote{it->first, it->second.shares};
}

std::optional<Quote> OrderBook::best_ask(std::uint16_t locate) const {
  const Levels &asks = books_[locate].asks;
  if (asks.empty())
    return std::nullopt;
  const auto it = asks.begin(); // lowest price
  return Quote{it->first, it->second.shares};
}

std::optional<std::uint16_t>
OrderBook::find_locate(std::string_view symbol) const {
  if (symbol.empty() || symbol.size() > 8)
    return std::nullopt;
  std::array<char, 8> key;
  key.fill(' '); // ITCH alpha fields are right-padded with spaces
  std::copy(symbol.begin(), symbol.end(), key.begin());

  // Linear scan of 65536 entries: fine for a one-off lookup, not for a hot path.
  for (std::size_t i = 0; i < symbols_.size(); ++i)
    if (symbols_[i] == key)
      return static_cast<std::uint16_t>(i);
  return std::nullopt;
}

bool OrderBook::validate() const {
  // Rebuild every level from scratch using only the order map...
  std::vector<Book> expected(books_.size());
  for (const auto &entry : orders_) {
    const Order &o = entry.second;
    Book &b = expected[o.locate];
    Level &lvl = (o.side == 'B' ? b.bids : b.asks)[o.price];
    lvl.shares += o.shares;
    ++lvl.orders;
  }
  // ...and it must match the incrementally maintained levels exactly
  // (including: no empty levels left behind).
  return expected == books_;
}

} // namespace itch
