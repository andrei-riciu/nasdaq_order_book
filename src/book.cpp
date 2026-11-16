#include "itch/book.hpp"
#include "itch/messages.hpp"

#include <algorithm>
#include <cassert>
#include <map>
#include <optional>
#include <vector>

namespace itch {

OrderBook::OrderBook() : books_(65536), symbols_(65536) {
  orders_.reserve(4'000'000);
}

Levels &OrderBook::levels(std::uint16_t locate, char side) {
  Book &b = books_[locate];
  return side == 'B' ? b.bids : b.asks;
}

namespace {

// Is price a closer to the top of the book than b, on this side?
bool better(char side, std::uint32_t a, std::uint32_t b) {
  return side == 'B' ? a > b : a < b;
}

// Scan from the back (best price) towards worse prices, skipping every level
// strictly better than `price`. Returns i such that:
//   - if i > 0 and lv[i-1].price == price, the level exists at i-1;
//   - otherwise a new level for `price` belongs at position i.
std::size_t scan(const Levels &lv, char side, std::uint32_t price) {
  std::size_t i = lv.size();
  while (i > 0 && better(side, lv[i - 1].price, price))
    --i;
  return i;
}

} // namespace

void OrderBook::add(std::uint64_t ref, std::uint16_t locate, char side,
                    std::uint32_t shares, std::uint32_t price) {
  const auto [it, inserted] =
      orders_.try_emplace(ref, Order{price, shares, locate, side});
  if (!inserted) {
    ++stats_.duplicate_refs; // keep the original; don't double-count shares
    return;
  }

  Levels &lvls = levels(locate, side);
  const std::size_t i = scan(lvls, side, price);
  if (i > 0 && lvls[i - 1].price == price) {
    lvls[i - 1].shares += shares;
    ++lvls[i - 1].orders;
  } else {
    lvls.insert(lvls.begin() + i, Level{price, 0, 0}); // new level at i
    lvls[i].shares += shares;
    ++lvls[i].orders;
  }
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

  Levels &lvls = levels(o.locate, o.side);
  const std::size_t i = scan(lvls, o.side, o.price);
  assert(i > 0 && lvls[i - 1].price == o.price &&
         "order exists but its level does not");
  lvls[i - 1].shares -= shares;
  o.shares -= shares;

  if (o.shares == 0) {
    if (--lvls[i - 1].orders == 0) {
      assert(lvls[i - 1].shares == 0);
      lvls.erase(lvls.begin() + (i - 1)); // no empty levels: back() is real
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
  const Levels &bids = books_[locate].bids; // reference: no copy
  if (bids.empty())
    return std::nullopt;
  const Level &top = bids.back(); // highest price
  return Quote{top.price, top.shares};
}

std::optional<Quote> OrderBook::best_ask(std::uint16_t locate) const {
  const Levels &asks = books_[locate].asks;
  if (asks.empty())
    return std::nullopt;
  const Level &top = asks.back(); // lowest price
  return Quote{top.price, top.shares};
}

std::optional<std::uint16_t>
OrderBook::find_locate(std::string_view symbol) const {
  if (symbol.empty() || symbol.size() > 8)
    return std::nullopt;
  std::array<char, 8> key;
  key.fill(' '); // ITCH alpha fields are right-padded with spaces
  std::copy(symbol.begin(), symbol.end(), key.begin());

  // Linear scan of 65536 entries: fine for a one-off lookup, not for a hot
  // path.
  for (std::size_t i = 0; i < symbols_.size(); ++i)
    if (symbols_[i] == key)
      return static_cast<std::uint16_t>(i);
  return std::nullopt;
}

bool OrderBook::validate() const {
  // Rebuild every level from the order map alone, using std::map: code that
  // shares nothing with scan()/add()/reduce(), so a bug there cannot hide by
  // being repeated here. One map per (locate, side); the map keeps prices sorted.
  using PriceMap = std::map<std::uint32_t, Level>;
  std::vector<std::array<PriceMap, 2>> rebuilt(books_.size()); // [0]=bids [1]=asks
  for (const auto &entry : orders_) {
    const Order &o = entry.second;
    Level &lvl = rebuilt[o.locate][o.side == 'B' ? 0 : 1][o.price];
    lvl.price = o.price;
    lvl.shares += o.shares;
    ++lvl.orders;
  }

  // Lay each one out like the real book (best price at the back) and compare.
  // Comparing whole vectors also checks the sort order and that no empty
  // level was left behind.
  for (std::size_t loc = 0; loc < books_.size(); ++loc) {
    const auto &[bids, asks] = rebuilt[loc];
    Book expected;
    for (const auto &kv : bids) // ascending: highest bid ends up at the back
      expected.bids.push_back(kv.second);
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) // descending
      expected.asks.push_back(it->second);
    if (expected != books_[loc])
      return false;
  }
  return true;
}

} // namespace itch
