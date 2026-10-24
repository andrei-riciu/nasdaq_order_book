#pragma once

// Test-only encoders: turn message structs back into ITCH 5.0 wire bytes.
// Written from the spec independently of the parsers, so a wrong offset in
// either one shows up as a round-trip failure.

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

#include "itch/messages.hpp"

namespace itch::test {

using Buffer = std::vector<std::uint8_t>;

class Bytes {
public:
  Bytes &u8(std::uint8_t x) {
    v_.push_back(x);
    return *this;
  }
  Bytes &ch(char c) { return u8(static_cast<std::uint8_t>(c)); }
  Bytes &u16(std::uint16_t x) {
    return u8(static_cast<std::uint8_t>(x >> 8)).u8(static_cast<std::uint8_t>(x));
  }
  Bytes &u32(std::uint32_t x) {
    return u16(static_cast<std::uint16_t>(x >> 16)).u16(static_cast<std::uint16_t>(x));
  }
  Bytes &u48(std::uint64_t x) {
    return u16(static_cast<std::uint16_t>(x >> 32)).u32(static_cast<std::uint32_t>(x));
  }
  Bytes &u64(std::uint64_t x) {
    return u32(static_cast<std::uint32_t>(x >> 32)).u32(static_cast<std::uint32_t>(x));
  }
  // Alpha field: left-justified, right-padded with spaces.
  Bytes &alpha(std::string_view s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
      ch(i < s.size() ? s[i] : ' ');
    return *this;
  }
  template <std::size_t N> Bytes &alpha(const std::array<char, N> &a) {
    for (char c : a)
      ch(c);
    return *this;
  }
  Buffer take() { return std::move(v_); }

private:
  Buffer v_;
};

inline std::array<char, 8> stock(std::string_view s) {
  std::array<char, 8> a;
  a.fill(' ');
  for (std::size_t i = 0; i < s.size() && i < a.size(); ++i)
    a[i] = s[i];
  return a;
}

inline Bytes header(char type, const Header &h) {
  Bytes b;
  b.ch(type).u16(h.locate).u16(h.tracking).u48(h.timestamp);
  return b;
}

inline Buffer encode(const SystemEvent &m) {
  return header('S', m.hdr).ch(m.event_code).take();
}

inline Buffer encode(const StockDirectory &m) {
  // Only the symbol is decoded; the remaining 20 bytes are filler here.
  return header('R', m.hdr).alpha(m.stock).alpha("", 20).take();
}

inline Buffer encode(const AddOrder &m) {
  return header('A', m.hdr)
      .u64(m.ref).ch(m.side).u32(m.shares).alpha(m.stock).u32(m.price)
      .take();
}

inline Buffer encode_with_mpid(const AddOrder &m, std::string_view mpid) {
  return header('F', m.hdr)
      .u64(m.ref).ch(m.side).u32(m.shares).alpha(m.stock).u32(m.price)
      .alpha(mpid, 4)
      .take();
}

inline Buffer encode(const OrderExecuted &m) {
  return header('E', m.hdr).u64(m.ref).u32(m.shares).u64(m.match).take();
}

inline Buffer encode(const OrderExecutedWithPrice &m) {
  return header('C', m.hdr)
      .u64(m.ref).u32(m.shares).u64(m.match).ch(m.printable).u32(m.price)
      .take();
}

inline Buffer encode(const OrderCancel &m) {
  return header('X', m.hdr).u64(m.ref).u32(m.shares).take();
}

inline Buffer encode(const OrderDelete &m) {
  return header('D', m.hdr).u64(m.ref).take();
}

inline Buffer encode(const OrderReplace &m) {
  return header('U', m.hdr)
      .u64(m.old_ref).u64(m.new_ref).u32(m.shares).u32(m.price)
      .take();
}

// Concatenate messages, each preceded by its 2-byte big-endian length, as in
// the Nasdaq sample files.
inline Buffer frame(std::initializer_list<Buffer> msgs) {
  Buffer out;
  for (const auto &m : msgs) {
    out.push_back(static_cast<std::uint8_t>(m.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(m.size()));
    out.insert(out.end(), m.begin(), m.end());
  }
  return out;
}

} // namespace itch::test
