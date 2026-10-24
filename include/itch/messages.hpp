#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace itch {
struct Header {
  std::uint16_t locate;
  std::uint16_t tracking;
  std::uint64_t timestamp; // 6 bytes on the wire, nanoseconds since midnight

  bool operator==(const Header &) const = default;
};

struct SystemEvent {
  static constexpr std::size_t length = 12;
  Header hdr;
  char event_code;

  bool operator==(const SystemEvent &) const = default;
};

struct StockDirectory {
  static constexpr std::size_t length = 39;
  Header hdr;
  std::array<char, 8> stock;

  bool operator==(const StockDirectory &) const = default;
};

// 'A' and 'F' share this struct: 'F' is 'A' plus a 4-byte MPID, which we ignore.
struct AddOrder {
  static constexpr std::size_t length = 36;
  static constexpr std::size_t length_mpid = 40;
  Header hdr;
  std::uint64_t ref;
  char side;
  std::uint32_t shares;
  std::array<char, 8> stock;
  std::uint32_t price;

  bool operator==(const AddOrder &) const = default;
};

struct OrderExecuted {
  static constexpr std::size_t length = 31;
  Header hdr;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;

  bool operator==(const OrderExecuted &) const = default;
};

struct OrderExecutedWithPrice {
  static constexpr std::size_t length = 36;
  Header hdr;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
  char printable;
  std::uint32_t price;

  bool operator==(const OrderExecutedWithPrice &) const = default;
};

struct OrderCancel {
  static constexpr std::size_t length = 23;
  Header hdr;
  std::uint64_t ref;
  std::uint32_t shares;

  bool operator==(const OrderCancel &) const = default;
};

struct OrderDelete {
  static constexpr std::size_t length = 19;
  Header hdr;
  std::uint64_t ref;

  bool operator==(const OrderDelete &) const = default;
};

struct OrderReplace {
  static constexpr std::size_t length = 35;
  Header hdr;
  std::uint64_t old_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;

  bool operator==(const OrderReplace &) const = default;
};
} // namespace itch
