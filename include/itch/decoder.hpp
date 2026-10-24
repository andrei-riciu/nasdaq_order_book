#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include "itch/messages.hpp"

namespace itch::detail {

template <class T> inline T load_be(const std::uint8_t *p) {
  T v;
  std::memcpy(&v, p, sizeof v); // safe for unaligned data; compiles to one load
  if constexpr (std::endian::native == std::endian::little)
    v = std::byteswap(v);
  return v;
}

inline std::uint64_t load_be48(const std::uint8_t *p) { // 6-byte timestamp
  return (std::uint64_t{load_be<std::uint16_t>(p)} << 32) |
         load_be<std::uint32_t>(p + 2);
}

inline std::array<char, 8> load_stock(const std::uint8_t *p) {
  std::array<char, 8> s;
  std::memcpy(s.data(), p, s.size());
  return s;
}

} // namespace itch::detail

namespace itch {

// Each parser takes a pointer to the message type byte (offset 0). Offsets
// below are from the TotalView-ITCH 5.0 spec. Designated initializers make

inline Header parse_header(const std::uint8_t *m) {
  using namespace detail;
  return {
      .locate = load_be<std::uint16_t>(m + 1),
      .tracking = load_be<std::uint16_t>(m + 3),
      .timestamp = load_be48(m + 5),
  };
}

inline SystemEvent parse_system_event(const std::uint8_t *m) {
  return {
      .hdr = parse_header(m),
      .event_code = static_cast<char>(m[11]),
  };
}

inline StockDirectory parse_stock_directory(const std::uint8_t *m) {
  return {
      .hdr = parse_header(m),
      .stock = detail::load_stock(m + 11),
  };
}

// Works for both 'A' and 'F'; the MPID at offset 36 is not read.
inline AddOrder parse_add_order(const std::uint8_t *m) {
  using namespace detail;
  return {
      .hdr = parse_header(m),
      .ref = load_be<std::uint64_t>(m + 11),
      .side = static_cast<char>(m[19]),
      .shares = load_be<std::uint32_t>(m + 20),
      .stock = load_stock(m + 24),
      .price = load_be<std::uint32_t>(m + 32),
  };
}

inline OrderExecuted parse_order_executed(const std::uint8_t *m) {
  using namespace detail;
  return {
      .hdr = parse_header(m),
      .ref = load_be<std::uint64_t>(m + 11),
      .shares = load_be<std::uint32_t>(m + 19),
      .match = load_be<std::uint64_t>(m + 23),
  };
}

inline OrderExecutedWithPrice
parse_order_executed_with_price(const std::uint8_t *m) {
  using namespace detail;
  return {
      .hdr = parse_header(m),
      .ref = load_be<std::uint64_t>(m + 11),
      .shares = load_be<std::uint32_t>(m + 19),
      .match = load_be<std::uint64_t>(m + 23),
      .printable = static_cast<char>(m[31]),
      .price = load_be<std::uint32_t>(m + 32),
  };
}

inline OrderCancel parse_order_cancel(const std::uint8_t *m) {
  using namespace detail;
  return {
      .hdr = parse_header(m),
      .ref = load_be<std::uint64_t>(m + 11),
      .shares = load_be<std::uint32_t>(m + 19),
  };
}

inline OrderDelete parse_order_delete(const std::uint8_t *m) {
  return {
      .hdr = parse_header(m),
      .ref = detail::load_be<std::uint64_t>(m + 11),
  };
}

inline OrderReplace parse_order_replace(const std::uint8_t *m) {
  using namespace detail;
  return {
      .hdr = parse_header(m),
      .old_ref = load_be<std::uint64_t>(m + 11),
      .new_ref = load_be<std::uint64_t>(m + 19),
      .shares = load_be<std::uint32_t>(m + 27),
      .price = load_be<std::uint32_t>(m + 31),
  };
}

} // namespace itch
