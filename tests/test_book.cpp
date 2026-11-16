#include <gtest/gtest.h>

#include <cstdint>
#include <algorithm>
#include <map>
#include <optional>
#include <ostream>
#include <random>
#include <vector>

#include "itch/book.hpp"
#include "itch_builders.hpp"

namespace itch {
// Lets gtest print a Quote in failure messages instead of raw bytes.
inline void PrintTo(const Quote &q, std::ostream *os) {
  *os << "{price=" << q.price << ", shares=" << q.shares << "}";
}
inline void PrintTo(const Level &l, std::ostream *os) {
  *os << "{price=" << l.price << ", orders=" << l.orders
      << ", shares=" << l.shares << "}";
}
} // namespace itch

using namespace itch;
using namespace itch::test;

namespace {

constexpr std::uint16_t kAapl = 1;
constexpr std::uint16_t kMsft = 2;

// Prices have 4 implied decimals: 1'500'000 = $150.0000.
constexpr std::uint32_t k150 = 1'500'000;
constexpr std::uint32_t k151 = 1'510'000;
constexpr std::uint32_t k152 = 1'520'000;

Header hdr(std::uint16_t locate = kAapl) {
  return {.locate = locate, .tracking = 0, .timestamp = 0};
}

AddOrder add(std::uint64_t ref, char side, std::uint32_t shares,
             std::uint32_t price, std::uint16_t locate = kAapl) {
  return {.hdr = hdr(locate), .ref = ref, .side = side, .shares = shares,
          .stock = stock("AAPL"), .price = price};
}
OrderExecuted exec(std::uint64_t ref, std::uint32_t shares) {
  return {.hdr = hdr(), .ref = ref, .shares = shares, .match = 0};
}
OrderExecutedWithPrice exec_px(std::uint64_t ref, std::uint32_t shares,
                               std::uint32_t price) {
  return {.hdr = hdr(), .ref = ref, .shares = shares, .match = 0,
          .printable = 'Y', .price = price};
}
OrderCancel cancel(std::uint64_t ref, std::uint32_t shares) {
  return {.hdr = hdr(), .ref = ref, .shares = shares};
}
OrderDelete del(std::uint64_t ref) { return {.hdr = hdr(), .ref = ref}; }
OrderReplace replace(std::uint64_t old_ref, std::uint64_t new_ref,
                     std::uint32_t shares, std::uint32_t price) {
  return {.hdr = hdr(), .old_ref = old_ref, .new_ref = new_ref,
          .shares = shares, .price = price};
}
StockDirectory directory(std::uint16_t locate, std::string_view sym) {
  return {.hdr = hdr(locate), .stock = stock(sym)};
}

bool clean(const BookStats &s) {
  return s.missing_refs == 0 && s.duplicate_refs == 0 && s.overfills == 0;
}

// The level at `price`, if there is one.
std::optional<Level> level_at(const Levels &lv, std::uint32_t price) {
  for (const Level &l : lv)
    if (l.price == price)
      return l;
  return std::nullopt;
}

// Prices in storage order, to check the layout (best price at the back).
std::vector<std::uint32_t> prices(const Levels &lv) {
  std::vector<std::uint32_t> p;
  for (const Level &l : lv)
    p.push_back(l.price);
  return p;
}

// Expected layout from a price-sorted map: bids ascending, asks descending.
Levels to_levels(const std::map<std::uint32_t, Level> &m, char side) {
  Levels lv;
  for (const auto &[price, l] : m)
    lv.push_back({.price = price, .orders = l.orders, .shares = l.shares});
  if (side == 'S')
    std::ranges::reverse(lv);
  return lv;
}

} // namespace

// ---------------------------------------------------------------------------
// Adds and top of book

TEST(OrderBook, EmptyBookHasNoQuotes) {
  OrderBook b;
  EXPECT_EQ(b.best_bid(kAapl), std::nullopt);
  EXPECT_EQ(b.best_ask(kAapl), std::nullopt);
  EXPECT_EQ(b.open_orders(), 0u);
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, AddsAtSamePriceAccumulate) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));
  b.on_add(add(2, 'B', 200, k150));

  EXPECT_EQ(level_at(b.book(kAapl).bids, k150),
            (Level{.price = k150, .orders = 2, .shares = 300}));
  EXPECT_EQ(b.best_bid(kAapl), (Quote{k150, 300}));
  EXPECT_EQ(b.best_ask(kAapl), std::nullopt);
  EXPECT_EQ(b.open_orders(), 2u);
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, BestBidIsHighestBestAskIsLowest) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));
  b.on_add(add(2, 'B', 200, k151)); // better bid
  b.on_add(add(3, 'S', 300, k152));
  b.on_add(add(4, 'S', 400, k151 + 5'000)); // better ask ($151.50)

  EXPECT_EQ(b.best_bid(kAapl), (Quote{k151, 200}));
  EXPECT_EQ(b.best_ask(kAapl), (Quote{k151 + 5'000, 400}));
  EXPECT_EQ(b.book(kAapl).bids.size(), 2u);
  EXPECT_EQ(b.book(kAapl).asks.size(), 2u);
  EXPECT_TRUE(b.validate());
}

// ---------------------------------------------------------------------------
// Level layout: sorted vectors, best price at the back

TEST(OrderBook, InsertsLevelsAtFrontMiddleAndBack) {
  OrderBook b;
  const Levels &bids = b.book(kAapl).bids;
  const Levels &asks = b.book(kAapl).asks;

  // Bids are stored ascending, so the best (highest) bid is at the back.
  b.on_add(add(1, 'B', 10, k151));
  b.on_add(add(2, 'B', 20, k152));          // better: new back
  b.on_add(add(3, 'B', 30, k150));          // worse than all: new front
  b.on_add(add(4, 'B', 40, k151 + 5'000));  // in between: middle
  b.on_add(add(5, 'B', 50, k151));          // existing level: no new entry
  EXPECT_EQ(prices(bids), (std::vector{k150, k151, k151 + 5'000, k152}));
  EXPECT_EQ(bids[1], (Level{.price = k151, .orders = 2, .shares = 60}));
  EXPECT_EQ(bids.front(), (Level{.price = k150, .orders = 1, .shares = 30}));
  EXPECT_EQ(b.best_bid(kAapl), (Quote{k152, 20}));

  // Asks are stored descending, so the best (lowest) ask is at the back.
  b.on_add(add(11, 'S', 10, k151));
  b.on_add(add(12, 'S', 20, k150));         // better: new back
  b.on_add(add(13, 'S', 30, k152));         // worse than all: new front
  b.on_add(add(14, 'S', 40, k150 + 5'000)); // in between: middle
  b.on_add(add(15, 'S', 50, k152));         // existing level at the front
  EXPECT_EQ(prices(asks), (std::vector{k152, k151, k150 + 5'000, k150}));
  EXPECT_EQ(asks.front(), (Level{.price = k152, .orders = 2, .shares = 80}));
  EXPECT_EQ(b.best_ask(kAapl), (Quote{k150, 20}));

  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, ErasesLevelsAnywhere) {
  OrderBook b;
  const Levels &bids = b.book(kAapl).bids;
  const Levels &asks = b.book(kAapl).asks;
  const std::uint32_t p[] = {k150, k150 + 2'500, k151, k151 + 2'500};
  for (std::uint64_t i = 0; i < 4; ++i) {
    b.on_add(add(1 + i, 'B', 100, p[i]));  // refs 1..4
    b.on_add(add(11 + i, 'S', 100, p[i])); // refs 11..14
  }

  b.on_order_delete(del(2)); // a middle bid
  EXPECT_EQ(prices(bids), (std::vector{p[0], p[2], p[3]}));
  b.on_order_delete(del(1)); // the worst bid (front)
  EXPECT_EQ(prices(bids), (std::vector{p[2], p[3]}));
  b.on_order_executed(exec(4, 100)); // the best bid (back), by execution
  EXPECT_EQ(prices(bids), (std::vector{p[2]}));
  EXPECT_EQ(b.best_bid(kAapl), (Quote{p[2], 100}));

  b.on_order_cancel(cancel(13, 100)); // a middle ask
  EXPECT_EQ(prices(asks), (std::vector{p[3], p[1], p[0]}));
  b.on_order_delete(del(14)); // the worst ask (front)
  EXPECT_EQ(prices(asks), (std::vector{p[1], p[0]}));
  b.on_order_delete(del(11)); // the best ask (back)
  EXPECT_EQ(prices(asks), (std::vector{p[1]}));
  EXPECT_EQ(b.best_ask(kAapl), (Quote{p[1], 100}));

  EXPECT_TRUE(clean(b.stats()));
  EXPECT_TRUE(b.validate());
}

// ---------------------------------------------------------------------------
// Reductions: E, C, X, D

TEST(OrderBook, PartialThenFullExecution) {
  OrderBook b;
  b.on_add(add(1, 'S', 100, k150));

  b.on_order_executed(exec(1, 30));
  EXPECT_EQ(b.best_ask(kAapl), (Quote{k150, 70}));
  EXPECT_EQ(b.open_orders(), 1u);

  b.on_order_executed(exec(1, 70));
  EXPECT_EQ(b.best_ask(kAapl), std::nullopt); // level erased, not left at 0
  EXPECT_TRUE(b.book(kAapl).asks.empty());
  EXPECT_EQ(b.open_orders(), 0u);
  EXPECT_TRUE(clean(b.stats()));
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, ExecutionWithPriceReducesAtRestingPrice) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));

  // Executed at a different price (e.g. a cross); the resting level is $150.
  b.on_order_executed_with_price(exec_px(1, 40, k151));

  EXPECT_EQ(b.best_bid(kAapl), (Quote{k150, 60}));
  EXPECT_EQ(level_at(b.book(kAapl).bids, k151), std::nullopt);
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, CancelIsPartial) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));
  b.on_order_cancel(cancel(1, 25));

  EXPECT_EQ(b.best_bid(kAapl), (Quote{k150, 75}));
  EXPECT_EQ(b.open_orders(), 1u);
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, DeleteRemovesOrderButKeepsSharedLevel) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));
  b.on_add(add(2, 'B', 200, k150));

  b.on_order_delete(del(1));
  EXPECT_EQ(level_at(b.book(kAapl).bids, k150),
            (Level{.price = k150, .orders = 1, .shares = 200}));

  b.on_order_delete(del(2));
  EXPECT_TRUE(b.book(kAapl).bids.empty());
  EXPECT_EQ(b.open_orders(), 0u);
  EXPECT_TRUE(clean(b.stats()));
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, DeleteAfterPartialExecutionRemovesTheRest) {
  OrderBook b;
  b.on_add(add(1, 'S', 100, k150));
  b.on_order_executed(exec(1, 40));
  b.on_order_delete(del(1)); // must remove 60, not the original 100

  EXPECT_TRUE(b.book(kAapl).asks.empty());
  EXPECT_EQ(b.stats().overfills, 0u);
  EXPECT_TRUE(b.validate());
}

// ---------------------------------------------------------------------------
// Replace

TEST(OrderBook, ReplaceMovesOrderToNewPriceAndSize) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));
  b.on_add(add(2, 'B', 50, k150));

  b.on_order_replace(replace(1, 10, 300, k151));

  EXPECT_EQ(level_at(b.book(kAapl).bids, k150),
            (Level{.price = k150, .orders = 1, .shares = 50}));
  EXPECT_EQ(b.best_bid(kAapl), (Quote{k151, 300}));
  EXPECT_EQ(b.open_orders(), 2u);

  // The old ref is gone, the new one is live.
  b.on_order_executed(exec(1, 1));
  EXPECT_EQ(b.stats().missing_refs, 1u);
  b.on_order_executed(exec(10, 100));
  EXPECT_EQ(b.best_bid(kAapl), (Quote{k151, 200}));
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, ReplaceKeepsSideAndSymbol) {
  OrderBook b;
  b.on_add(add(1, 'S', 100, k152, kMsft));

  // Replace carries no side or locate: both must come from the old order.
  OrderReplace u = replace(1, 2, 80, k151);
  u.hdr.locate = kMsft;
  b.on_order_replace(u);

  EXPECT_EQ(b.best_ask(kMsft), (Quote{k151, 80}));
  EXPECT_EQ(b.best_bid(kMsft), std::nullopt);
  EXPECT_EQ(b.best_ask(kAapl), std::nullopt);
  EXPECT_TRUE(b.validate());
}

// ---------------------------------------------------------------------------
// Bad input is counted, never corrupts the book

TEST(OrderBook, UnknownRefsAreCountedAndIgnored) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));

  b.on_order_executed(exec(99, 10));
  b.on_order_executed_with_price(exec_px(99, 10, k150));
  b.on_order_cancel(cancel(99, 10));
  b.on_order_delete(del(99));
  b.on_order_replace(replace(99, 100, 10, k150));

  EXPECT_EQ(b.stats().missing_refs, 5u);
  EXPECT_EQ(b.best_bid(kAapl), (Quote{k150, 100}));
  EXPECT_EQ(b.open_orders(), 1u); // the replace did not add ref 100
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, OverfillIsClampedAndCounted) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));
  b.on_add(add(2, 'B', 50, k150));

  b.on_order_executed(exec(1, 150)); // more than order 1 has

  EXPECT_EQ(b.stats().overfills, 1u);
  // Only order 1's 100 shares came off; order 2 is untouched.
  EXPECT_EQ(level_at(b.book(kAapl).bids, k150),
            (Level{.price = k150, .orders = 1, .shares = 50}));
  EXPECT_EQ(b.open_orders(), 1u);
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, DuplicateAddIsCountedAndIgnored) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));
  b.on_add(add(1, 'S', 999, k152)); // same ref again

  EXPECT_EQ(b.stats().duplicate_refs, 1u);
  EXPECT_EQ(b.best_bid(kAapl), (Quote{k150, 100}));
  EXPECT_EQ(b.best_ask(kAapl), std::nullopt);
  EXPECT_EQ(b.open_orders(), 1u);
  EXPECT_TRUE(b.validate());
}

// ---------------------------------------------------------------------------
// Symbols

TEST(OrderBook, SymbolsAreKeptApart) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150, kAapl));
  b.on_add(add(2, 'B', 200, k151, kMsft));

  EXPECT_EQ(b.best_bid(kAapl), (Quote{k150, 100}));
  EXPECT_EQ(b.best_bid(kMsft), (Quote{k151, 200}));

  b.on_order_delete(del(2)); // the delete's locate is ignored; the ref decides
  EXPECT_EQ(b.best_bid(kMsft), std::nullopt);
  EXPECT_EQ(b.best_bid(kAapl), (Quote{k150, 100}));
  EXPECT_TRUE(b.validate());
}

TEST(OrderBook, FindLocateUsesStockDirectory) {
  OrderBook b;
  b.on_stock_directory(directory(kAapl, "AAPL"));
  b.on_stock_directory(directory(kMsft, "MSFT"));

  EXPECT_EQ(b.find_locate("AAPL"), kAapl);
  EXPECT_EQ(b.find_locate("MSFT"), kMsft);
  EXPECT_EQ(b.find_locate("AAP"), std::nullopt); // no prefix matches
  EXPECT_EQ(b.find_locate("ZZZZ"), std::nullopt);
  EXPECT_EQ(b.find_locate(""), std::nullopt);
  EXPECT_EQ(b.find_locate("TOOLONGXX"), std::nullopt);
}

TEST(OrderBook, PeakOrdersTracksHighWaterMark) {
  OrderBook b;
  b.on_add(add(1, 'B', 100, k150));
  b.on_add(add(2, 'B', 100, k150));
  b.on_add(add(3, 'S', 100, k152));
  b.on_order_delete(del(1));
  b.on_order_delete(del(2));

  EXPECT_EQ(b.open_orders(), 1u);
  EXPECT_EQ(b.stats().peak_orders, 3u);
}

// ---------------------------------------------------------------------------
// End to end: wire bytes -> decode() -> OrderBook

TEST(OrderBook, EndToEndThroughDecoder) {
  const auto buf = frame({
      encode(directory(kAapl, "AAPL")),
      encode(add(1, 'B', 100, k150)),
      encode_with_mpid(add(2, 'S', 200, k152), "GSCO"),
      encode(exec(1, 40)),
      encode(replace(2, 3, 150, k151)),
      encode(cancel(3, 50)),
  });
  OrderBook b;
  const auto res = decode(std::span<const std::uint8_t>(buf), b);

  EXPECT_EQ(res.messages, 6u);
  EXPECT_EQ(res.bad_length, 0u);
  EXPECT_EQ(b.find_locate("AAPL"), kAapl);
  EXPECT_EQ(b.best_bid(kAapl), (Quote{k150, 60}));
  EXPECT_EQ(b.best_ask(kAapl), (Quote{k151, 100}));
  EXPECT_TRUE(clean(b.stats()));
  EXPECT_TRUE(b.validate());
}

// ---------------------------------------------------------------------------
// Randomized differential test against a deliberately naive model:
// a plain map of live orders, with levels recomputed from scratch.

TEST(OrderBook, MatchesNaiveModelOnRandomFlow) {
  struct Naive {
    std::uint16_t locate;
    char side;
    std::uint32_t price;
    std::uint32_t shares;
  };
  std::map<std::uint64_t, Naive> model;
  std::vector<std::uint64_t> refs; // may hold dead refs; pruned lazily
  std::mt19937_64 rng(20200130);   // fixed seed: failures are reproducible
  auto pick = [&](std::uint64_t n) { return rng() % n; };

  auto expected_book = [&](std::uint16_t locate) {
    std::map<std::uint32_t, Level> bids, asks;
    for (const auto &[ref, o] : model)
      if (o.locate == locate) {
        Level &l = (o.side == 'B' ? bids : asks)[o.price];
        l.shares += o.shares;
        ++l.orders;
      }
    return Book{.bids = to_levels(bids, 'B'), .asks = to_levels(asks, 'S')};
  };

  OrderBook b;
  std::uint64_t next_ref = 1;

  for (int step = 0; step < 20'000; ++step) {
    // Find a live ref (if any) for the non-add operations.
    std::uint64_t ref = 0;
    while (!refs.empty()) {
      const std::size_t i = pick(refs.size());
      if (model.contains(refs[i])) {
        ref = refs[i];
        break;
      }
      refs[i] = refs.back();
      refs.pop_back();
    }

    const auto op = pick(100);
    if (ref == 0 || op < 40) {
      const Naive o{.locate = static_cast<std::uint16_t>(1 + pick(3)),
                    .side = pick(2) ? 'B' : 'S',
                    .price = static_cast<std::uint32_t>(1'000'000 + 100 * pick(20)),
                    .shares = static_cast<std::uint32_t>(1 + pick(500))};
      b.on_add(add(next_ref, o.side, o.shares, o.price, o.locate));
      model[next_ref] = o;
      refs.push_back(next_ref++);
      continue;
    }

    Naive &o = model.at(ref);
    const auto qty = static_cast<std::uint32_t>(1 + pick(o.shares)); // may be all
    if (op < 60) {
      b.on_order_executed(exec(ref, qty));
    } else if (op < 70) {
      b.on_order_executed_with_price(exec_px(ref, qty, o.price + 100));
    } else if (op < 80) {
      b.on_order_cancel(cancel(ref, qty));
    } else if (op < 90) {
      b.on_order_delete(del(ref));
      model.erase(ref);
      continue;
    } else {
      const Naive n{.locate = o.locate, .side = o.side,
                    .price = static_cast<std::uint32_t>(1'000'000 + 100 * pick(20)),
                    .shares = static_cast<std::uint32_t>(1 + pick(500))};
      b.on_order_replace(replace(ref, next_ref, n.shares, n.price));
      model.erase(ref);
      model[next_ref] = n;
      refs.push_back(next_ref++);
      continue;
    }
    o.shares -= qty;
    if (o.shares == 0)
      model.erase(ref);

    if (step % 1'000 == 0) {
      for (std::uint16_t loc = 1; loc <= 3; ++loc) {
        ASSERT_EQ(b.book(loc), expected_book(loc)) << "step " << step;
      }
    }
  }

  for (std::uint16_t loc = 1; loc <= 3; ++loc)
    EXPECT_EQ(b.book(loc), expected_book(loc));
  EXPECT_EQ(b.open_orders(), model.size());
  EXPECT_TRUE(clean(b.stats()));
  EXPECT_TRUE(b.validate());
}
