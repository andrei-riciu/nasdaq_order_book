#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "itch/decoder.hpp"
namespace itch {

struct Level {
  std::uint32_t price = 0;
  std::uint32_t orders = 0; // how many orders make up that total;
  std::uint64_t shares = 0; // total shares resting at this price
  bool operator==(const Level &) const = default;
};
// struct Level l1,l2;
// if(l1==l2) instead of if(l1.shares==l2.shares && l1.orders==l2.orders)

// Sorted so the BEST price is at the back, where almost all activity is:
//   bids ascending  [150.90, 151.00, 151.10]  back() = highest = best bid
//   asks descending [151.40, 151.30, 151.25]  back() = lowest  = best ask
using Levels = std::vector<Level>;

struct Book {
  Levels bids;
  Levels asks;

  bool operator==(const Book &) const = default;
};

struct Quote {
  std::uint32_t price;
  std::uint64_t shares;

  bool operator==(const Quote &) const = default;
};

struct BookStats {
  std::uint64_t missing_refs = 0;   // E/C/X/D/U for an order we don't have
  std::uint64_t duplicate_refs = 0; // add for a ref that already exists
  std::uint64_t overfills = 0;      // reduce by more shares than the order has
  std::size_t peak_orders = 0;      // largest number of live orders seen
};

class OrderBook : public NullHandler {
public:
  OrderBook();

  // Handlers called by decode(); everything else falls through to NullHandler.
  void on_stock_directory(const StockDirectory &m);
  void on_add(const AddOrder &m);
  void on_order_executed(const OrderExecuted &m);
  void on_order_executed_with_price(const OrderExecutedWithPrice &m);
  void on_order_cancel(const OrderCancel &m);
  void on_order_delete(const OrderDelete &m);
  void on_order_replace(const OrderReplace &m);

  // Queries
  const Book &book(std::uint16_t locate) const { return books_[locate]; }
  std::optional<Quote> best_bid(std::uint16_t locate) const;
  std::optional<Quote> best_ask(std::uint16_t locate) const;
  std::optional<std::uint16_t> find_locate(std::string_view symbol) const;
  std::size_t open_orders() const { return orders_.size(); }
  const BookStats &stats() const { return stats_; }

  // Rebuilds every level from the order map and compares. Slow (O(orders));
  // for tests and end-of-replay checks.
  bool validate() const;

private:
  struct Order {
    std::uint32_t price;
    std::uint32_t shares;
    std::uint16_t locate;
    char side; // 'B' or 'S'
  };

  void add(std::uint64_t ref, std::uint16_t locate, char side,
           std::uint32_t shares, std::uint32_t price);
  void reduce(std::uint64_t ref, std::uint32_t shares);
  Levels &levels(std::uint16_t locate, char side);

  std::vector<Book> books_;                  // indexed by locate code
  std::vector<std::array<char, 8>> symbols_; // locate -> ticker, from 'R'
  std::unordered_map<std::uint64_t, Order> orders_;
  BookStats stats_;
};
} // namespace itch
