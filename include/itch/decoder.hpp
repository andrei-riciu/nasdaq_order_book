#pragma once

#include "itch/messages.hpp"
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>

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
// below are from the TotalView-ITCH 5.0 spec.
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
// Does nothing for every message; inherit from it and override only what you
// need.
struct NullHandler {
  void on_system_event(const SystemEvent &) {}
  void on_add(const AddOrder &) {}
  void on_stock_directory(const StockDirectory &) {}
  void on_order_executed(const OrderExecuted &) {}
  void on_order_executed_with_price(const OrderExecutedWithPrice &) {}
  void on_order_cancel(const OrderCancel &) {}
  void on_order_delete(const OrderDelete &) {}
  void on_order_replace(const OrderReplace &) {}
  void on_unknown(char /*type*/, std::size_t /*len*/) {}
};

struct DecodeResult {
  std::size_t bytes_consumed;
  std::size_t messages;
  std::size_t bad_length;
};

template <class Handler>
DecodeResult decode(std::span<const std::uint8_t> buf, Handler &h) {
  std::size_t pos = 0, n = 0, bad = 0;

  while (pos + 2 <= buf.size()) {
    const auto len = detail::load_be<std::uint16_t>(buf.data() + pos);
    if (pos + 2 + len > buf.size())
      break;
    if (len == 0) {
      ++bad;
      pos += 2;
      continue;
    }
    const std::uint8_t *m = buf.data() + pos + 2;
    switch (m[0]) {
    case 'A':
      if (len != AddOrder::length)
        ++bad;
      else
        h.on_add(parse_add_order(m));
      break;
    case 'F':
      if (len != AddOrder::length_mpid)
        ++bad;
      else
        h.on_add(parse_add_order(m));
      break;
    case 'S':
      if (len != SystemEvent::length)
        ++bad;
      else
        h.on_system_event(parse_system_event(m));
      break;
    case 'R':
      if (len != StockDirectory::length)
        ++bad;
      else
        h.on_stock_directory(parse_stock_directory(m));
      break;
    case 'E':
      if (len != OrderExecuted::length)
        ++bad;
      else
        h.on_order_executed(parse_order_executed(m));
      break;
    case 'C':
      if (len != OrderExecutedWithPrice::length)
        ++bad;
      else
        h.on_order_executed_with_price(parse_order_executed_with_price(m));
      break;
    case 'X':
      if (len != OrderCancel::length)
        ++bad;
      else
        h.on_order_cancel(parse_order_cancel(m));
      break;
    case 'D':
      if (len != OrderDelete::length)
        ++bad;
      else
        h.on_order_delete(parse_order_delete(m));
      break;
    case 'U':
      if (len != OrderReplace::length)
        ++bad;
      else
        h.on_order_replace(parse_order_replace(m));
      break;
    default:
      h.on_unknown(static_cast<char>(m[0]), len);
      break;
    }
    pos += 2 + len;
    ++n;
  }
  return {pos, n, bad};
}

} // namespace itch
