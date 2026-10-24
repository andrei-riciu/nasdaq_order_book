#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace itch {
struct Header {
  std::uint16_t locate;
  std::uint16_t tracking;
  std::uint64_t timestamp; // 6 bytes on the wire, nanoseconds since midnight
};

struct SystemEvent {
  static constexpr std::size_t length = 12;
  Header hdr;
  char event_code;
};

struct StockDirectory {
  static constexpr std::size_t length = 39;
  Header hdr;
  std::array<char, 8> stock;
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
};

struct OrderExecuted {
  static constexpr std::size_t length = 31;
  Header hdr;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
};

struct OrderExecutedWithPrice {
  static constexpr std::size_t length = 36;
  Header hdr;
  std::uint64_t ref;
  std::uint32_t shares;
  std::uint64_t match;
  char printable;
  std::uint32_t price;
};

struct OrderCancel {
  static constexpr std::size_t length = 23;
  Header hdr;
  std::uint64_t ref;
  std::uint32_t shares;
};

struct OrderDelete {
  static constexpr std::size_t length = 19;
  Header hdr;
  std::uint64_t ref;
};

struct OrderReplace {
  static constexpr std::size_t length = 35;
  Header hdr;
  std::uint64_t old_ref;
  std::uint64_t new_ref;
  std::uint32_t shares;
  std::uint32_t price;
};
} // namespace itch
