#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "itch/decoder.hpp"
#include "itch_builders.hpp"

using namespace itch;
using namespace itch::test;

namespace {

// Every field gets a distinct, non-symmetric value so a swapped or shifted
// offset produces a wrong number instead of an accidental match.
constexpr Header kHdr{.locate = 0x1234, .tracking = 0x5678, .timestamp = 0x0A0B0C0D0E0F};

// Records every callback, in order, so tests can check both what was decoded
// and which handler it went to.
struct Recorder : NullHandler {
  std::string types; // one char per dispatched message, e.g. "SRAE"
  std::vector<SystemEvent> system_events;
  std::vector<StockDirectory> directories;
  std::vector<AddOrder> adds;
  std::vector<OrderExecuted> executions;
  std::vector<OrderExecutedWithPrice> executions_with_price;
  std::vector<OrderCancel> cancels;
  std::vector<OrderDelete> deletes;
  std::vector<OrderReplace> replaces;
  std::vector<std::pair<char, std::size_t>> unknown;

  void on_system_event(const SystemEvent &m) { types += 'S'; system_events.push_back(m); }
  void on_stock_directory(const StockDirectory &m) { types += 'R'; directories.push_back(m); }
  void on_add(const AddOrder &m) { types += 'A'; adds.push_back(m); }
  void on_order_executed(const OrderExecuted &m) { types += 'E'; executions.push_back(m); }
  void on_order_executed_with_price(const OrderExecutedWithPrice &m) {
    types += 'C';
    executions_with_price.push_back(m);
  }
  void on_order_cancel(const OrderCancel &m) { types += 'X'; cancels.push_back(m); }
  void on_order_delete(const OrderDelete &m) { types += 'D'; deletes.push_back(m); }
  void on_order_replace(const OrderReplace &m) { types += 'U'; replaces.push_back(m); }
  void on_unknown(char type, std::size_t len) { types += '?'; unknown.emplace_back(type, len); }
};

DecodeResult run(const Buffer &buf, Recorder &rec) {
  return decode(std::span<const std::uint8_t>(buf), rec);
}

// Sample messages reused across tests.
const SystemEvent kStartOfMessages{.hdr = kHdr, .event_code = 'O'};
const AddOrder kAdd{.hdr = kHdr, .ref = 0x0102030405060708, .side = 'B',
                    .shares = 100, .stock = stock("AAPL"), .price = 1'234'500};
const OrderDelete kDelete{.hdr = kHdr, .ref = 0x1112131415161718};

} // namespace

// ---------------------------------------------------------------------------
// Byte helpers

TEST(LoadBe, ReadsBigEndianIntegers) {
  const std::uint8_t b[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  EXPECT_EQ(detail::load_be<std::uint16_t>(b), 0x0102u);
  EXPECT_EQ(detail::load_be<std::uint32_t>(b), 0x01020304u);
  EXPECT_EQ(detail::load_be<std::uint64_t>(b), 0x0102030405060708u);
}

TEST(LoadBe, ReadsUnalignedAddresses) {
  const std::uint8_t b[] = {0xFF, 0x12, 0x34, 0x56, 0x78};
  EXPECT_EQ(detail::load_be<std::uint32_t>(b + 1), 0x12345678u);
}

TEST(LoadBe48, ReadsSixByteTimestamp) {
  const std::uint8_t b[] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0xEE};
  EXPECT_EQ(detail::load_be48(b), 0x0A0B0C0D0E0Fu); // trailing byte not read
}

TEST(LoadBe48, MaxValue) {
  const std::uint8_t b[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_EQ(detail::load_be48(b), 0xFFFF'FFFF'FFFFu);
}

// ---------------------------------------------------------------------------
// Parsers against hand-written bytes (independent of the test encoders)

TEST(Parse, AddOrderFromLiteralBytes) {
  const std::uint8_t m[] = {
      'A',                                            // 0  type
      0x12, 0x34,                                     // 1  locate
      0x56, 0x78,                                     // 3  tracking
      0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,             // 5  timestamp
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, // 11 order ref
      'B',                                            // 19 side
      0x00, 0x00, 0x00, 0x64,                         // 20 shares = 100
      'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',         // 24 stock
      0x00, 0x12, 0xD6, 0x44,                         // 32 price = 123.4500
  };
  static_assert(sizeof m == AddOrder::length);
  EXPECT_EQ(parse_add_order(m), kAdd);
}

// ---------------------------------------------------------------------------
// Round trips: encode a struct, decode the frame, get the same struct back

TEST(Decode, SystemEvent) {
  Recorder rec;
  run(frame({encode(kStartOfMessages)}), rec);
  ASSERT_EQ(rec.types, "S");
  EXPECT_EQ(rec.system_events[0], kStartOfMessages);
}

TEST(Decode, StockDirectory) {
  const StockDirectory m{.hdr = kHdr, .stock = stock("MSFT")};
  Recorder rec;
  run(frame({encode(m)}), rec);
  ASSERT_EQ(rec.types, "R");
  EXPECT_EQ(rec.directories[0], m);
}

TEST(Decode, AddOrder) {
  Recorder rec;
  run(frame({encode(kAdd)}), rec);
  ASSERT_EQ(rec.types, "A");
  EXPECT_EQ(rec.adds[0], kAdd);
}

TEST(Decode, AddOrderWithMpidGoesToOnAdd) {
  AddOrder m = kAdd;
  m.side = 'S';
  Recorder rec;
  const auto res = run(frame({encode_with_mpid(m, "GSCO")}), rec);
  ASSERT_EQ(rec.types, "A");
  EXPECT_EQ(rec.adds[0], m);
  EXPECT_EQ(res.bad_length, 0u);
}

TEST(Decode, OrderExecuted) {
  const OrderExecuted m{.hdr = kHdr, .ref = 0x0102030405060708, .shares = 30,
                        .match = 0x2122232425262728};
  Recorder rec;
  run(frame({encode(m)}), rec);
  ASSERT_EQ(rec.types, "E");
  EXPECT_EQ(rec.executions[0], m);
}

TEST(Decode, OrderExecutedWithPrice) {
  const OrderExecutedWithPrice m{.hdr = kHdr, .ref = 0x0102030405060708, .shares = 20,
                                 .match = 0x3132333435363738, .printable = 'N',
                                 .price = 1'234'400};
  Recorder rec;
  run(frame({encode(m)}), rec);
  ASSERT_EQ(rec.types, "C");
  EXPECT_EQ(rec.executions_with_price[0], m);
}

TEST(Decode, OrderCancel) {
  const OrderCancel m{.hdr = kHdr, .ref = 0x0102030405060708, .shares = 50};
  Recorder rec;
  run(frame({encode(m)}), rec);
  ASSERT_EQ(rec.types, "X");
  EXPECT_EQ(rec.cancels[0], m);
}

TEST(Decode, OrderDelete) {
  Recorder rec;
  run(frame({encode(kDelete)}), rec);
  ASSERT_EQ(rec.types, "D");
  EXPECT_EQ(rec.deletes[0], kDelete);
}

TEST(Decode, OrderReplace) {
  const OrderReplace m{.hdr = kHdr, .old_ref = 0x0102030405060708,
                       .new_ref = 0x4142434445464748, .shares = 150, .price = 1'234'600};
  Recorder rec;
  run(frame({encode(m)}), rec);
  ASSERT_EQ(rec.types, "U");
  EXPECT_EQ(rec.replaces[0], m);
}

// ---------------------------------------------------------------------------
// Framing and malformed input

TEST(Decode, EmptyBuffer) {
  Recorder rec;
  const auto res = run({}, rec);
  EXPECT_EQ(res.bytes_consumed, 0u);
  EXPECT_EQ(res.messages, 0u);
  EXPECT_EQ(rec.types, "");
}

TEST(Decode, MessagesArriveInOrderAndConsumeWholeBuffer) {
  const auto buf = frame({encode(kStartOfMessages), encode(kAdd), encode(kDelete),
                          encode(SystemEvent{.hdr = kHdr, .event_code = 'C'})});
  Recorder rec;
  const auto res = run(buf, rec);
  EXPECT_EQ(rec.types, "SADS");
  EXPECT_EQ(res.messages, 4u);
  EXPECT_EQ(res.bytes_consumed, buf.size());
  EXPECT_EQ(res.bad_length, 0u);
  EXPECT_EQ(rec.system_events.back().event_code, 'C');
}

TEST(Decode, UnknownTypeIsSkipped) {
  Buffer trade = header('P', kHdr).alpha("", 33).take(); // 44-byte trade message
  const auto buf = frame({encode(kAdd), trade, encode(kDelete)});
  Recorder rec;
  const auto res = run(buf, rec);
  EXPECT_EQ(rec.types, "A?D");
  ASSERT_EQ(rec.unknown.size(), 1u);
  EXPECT_EQ(rec.unknown[0], std::make_pair('P', std::size_t{44}));
  EXPECT_EQ(res.bytes_consumed, buf.size());
}

TEST(Decode, WrongLengthForKnownTypeIsCountedNotDispatched) {
  Buffer long_delete = encode(kDelete);
  long_delete.push_back(0x00); // 20 bytes instead of 19
  const auto buf = frame({long_delete, encode(kAdd)});
  Recorder rec;
  const auto res = run(buf, rec);
  EXPECT_EQ(rec.types, "A");
  EXPECT_EQ(res.bad_length, 1u);
  EXPECT_EQ(res.bytes_consumed, buf.size());
}

TEST(Decode, TruncatedFinalMessageIsNotRead) {
  const auto whole = frame({encode(kAdd)});
  Buffer buf = whole;
  const auto partial = frame({encode(kDelete)});
  // Cut exactly one byte: the tightest case, catches off-by-one/two bounds checks.
  buf.insert(buf.end(), partial.begin(), partial.end() - 1);
  Recorder rec;
  const auto res = run(buf, rec);
  EXPECT_EQ(rec.types, "A");
  EXPECT_EQ(res.bytes_consumed, whole.size());
}

TEST(Decode, LoneLengthByteAtEndIsNotRead) {
  Buffer buf = frame({encode(kAdd)});
  buf.push_back(0x00); // half of a length prefix
  Recorder rec;
  const auto res = run(buf, rec);
  EXPECT_EQ(rec.types, "A");
  EXPECT_EQ(res.bytes_consumed, buf.size() - 1);
}

TEST(Decode, ZeroLengthFrameIsCountedAndSkipped) {
  Buffer buf = {0x00, 0x00};
  const auto rest = frame({encode(kAdd)});
  buf.insert(buf.end(), rest.begin(), rest.end());
  Recorder rec;
  const auto res = run(buf, rec);
  EXPECT_EQ(rec.types, "A");
  EXPECT_EQ(res.bad_length, 1u);
  EXPECT_EQ(res.bytes_consumed, buf.size());
}

TEST(Decode, ZeroLengthFrameAtEndOfBuffer) {
  // Regression: used to read the type byte one past the end of the buffer.
  const Buffer buf = {0x00, 0x00};
  Recorder rec;
  const auto res = run(buf, rec);
  EXPECT_EQ(rec.types, "");
  EXPECT_EQ(res.bad_length, 1u);
  EXPECT_EQ(res.bytes_consumed, 2u);
}
