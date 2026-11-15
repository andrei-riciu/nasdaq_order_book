#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <numeric>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "itch/book.hpp"
#include "itch/decoder.hpp"
#include "itch/mapped_file.hpp"

namespace {

struct CountingHandler : itch::NullHandler {
  std::array<std::uint64_t, 256> counts{}; // {} sets them all to zero
  std::uint64_t last_ts = 0;
  std::uint64_t ts_regressions = 0;
  std::vector<std::pair<char, std::uint64_t>>
      system_events; // (code, timestamp)

  void seen(char type, const itch::Header &h) {
    ++counts[static_cast<unsigned char>(type)];
    if (h.timestamp < last_ts)
      ++ts_regressions;
    last_ts = h.timestamp;
  }

  void on_system_event(const itch::SystemEvent &m) {
    seen('S', m.hdr);
    system_events.emplace_back(m.event_code, m.hdr.timestamp);
  }
  void on_stock_directory(const itch::StockDirectory &m) { seen('R', m.hdr); }
  void on_add(const itch::AddOrder &m) { seen('A', m.hdr); } // A and F together
  void on_order_executed(const itch::OrderExecuted &m) { seen('E', m.hdr); }
  void on_order_executed_with_price(const itch::OrderExecutedWithPrice &m) {
    seen('C', m.hdr);
  }
  void on_order_cancel(const itch::OrderCancel &m) { seen('X', m.hdr); }
  void on_order_delete(const itch::OrderDelete &m) { seen('D', m.hdr); }
  void on_order_replace(const itch::OrderReplace &m) { seen('U', m.hdr); }

  void on_unknown(char type, std::size_t) {
    ++counts[static_cast<unsigned char>(type)];
  }
};

// Nanoseconds since midnight -> HH:MM:SS.nnnnnnnnn
std::string format_ts(std::uint64_t ns) {
  const std::uint64_t s = ns / 1'000'000'000;
  return std::format("{:02}:{:02}:{:02}.{:09}", s / 3600, s / 60 % 60, s % 60,
                     ns % 1'000'000'000);
}

// 1234567 -> "1,234,567"
std::string with_commas(std::uint64_t n) {
  std::string s = std::to_string(n);
  for (std::size_t i = s.size(); i > 3; i -= 3)
    s.insert(i - 3, ",");
  return s;
}

std::string_view event_name(char code) {
  switch (code) {
  case 'O':
    return "start of messages";
  case 'S':
    return "start of system hours";
  case 'Q':
    return "start of market hours";
  case 'M':
    return "end of market hours";
  case 'E':
    return "end of system hours";
  case 'C':
    return "end of messages";
  default:
    return "?";
  }
}

std::string_view type_name(char type) {
  switch (type) {
  case 'S':
    return "system event";
  case 'R':
    return "stock directory";
  case 'H':
    return "stock trading action";
  case 'Y':
    return "Reg SHO restriction";
  case 'L':
    return "market participant position";
  case 'V':
    return "MWCB decline level";
  case 'W':
    return "MWCB status";
  case 'K':
    return "IPO quoting period";
  case 'J':
    return "LULD auction collar";
  case 'h':
    return "operational halt";
  case 'A':
    return "add order (A + F)";
  case 'E':
    return "order executed";
  case 'C':
    return "order executed with price";
  case 'X':
    return "order cancel";
  case 'D':
    return "order delete";
  case 'U':
    return "order replace";
  case 'P':
    return "trade (non-cross)";
  case 'Q':
    return "cross trade";
  case 'B':
    return "broken trade";
  case 'I':
    return "NOII";
  case 'N':
    return "RPII";
  case 'O':
    return "direct listing price discovery";
  default:
    return "";
  }
}

// "$123.4500" from ITCH's 4-implied-decimal integer price.
std::string format_price(std::uint32_t p) {
  return std::format("{}.{:04}", p / 10'000, p % 10'000);
}

// OrderBook plus sanity checks that only make sense on a real feed.
//
// decode<CheckedBook> calls CheckedBook::on_add etc. by name, exactly like
// OrderBook hides NullHandler's empty versions. Each override forwards to the
// real book first, then looks at the result. Handlers we don't override
// (E/C/X/D, R) go straight to OrderBook's versions.
class CheckedBook : public itch::OrderBook {
public:
  explicit CheckedBook(std::vector<std::string_view> watch)
      : watch_(std::move(watch)) {}

  struct Snapshot {
    std::string_view symbol;
    std::optional<itch::Quote> bid, ask;
  };

  // Results, read by main() after the replay.
  std::vector<std::pair<char, std::uint64_t>> system_events;
  std::uint64_t locked = 0;  // best bid == best ask after an add/replace
  std::uint64_t crossed = 0; // best bid >  best ask after an add/replace
  std::vector<Snapshot> at_close; // top of book for watch_ at 'M' (16:00)

  void on_system_event(const itch::SystemEvent &m) {
    system_events.emplace_back(m.event_code, m.hdr.timestamp);
    if (m.event_code == 'M') // end of market hours: photograph the books
      for (std::string_view sym : watch_)
        if (const auto loc = find_locate(sym))
          at_close.push_back({sym, best_bid(*loc), best_ask(*loc)});
  }

  // Only adds and replaces can put a new price on the book, so only they can
  // lock or cross it. Executions/cancels/deletes only remove shares.
  void on_add(const itch::AddOrder &m) {
    OrderBook::on_add(m);
    check(m.hdr.locate);
  }
  void on_order_replace(const itch::OrderReplace &m) {
    OrderBook::on_order_replace(m);
    check(m.hdr.locate);
  }

private:
  void check(std::uint16_t locate) {
    const auto bid = best_bid(locate);
    const auto ask = best_ask(locate);
    if (!bid || !ask)
      return;
    if (bid->price == ask->price)
      ++locked;
    else if (bid->price > ask->price)
      ++crossed;
  }

  std::vector<std::string_view> watch_;
};


int run_count(const char *path) {
  const itch::MappedFile file(path);
  const auto buf = file.bytes();
  CountingHandler h;

  // Time only the decode, not the mapping.
  const auto t0 = std::chrono::steady_clock::now();
  const auto res = itch::decode(buf, h);
  const auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();

  // Every dispatched message is counted exactly once, so this only differs
  // from res.messages if a handler override is misspelled (or bad_length >
  // 0).
  const std::uint64_t counted =
      std::accumulate(h.counts.begin(), h.counts.end(), std::uint64_t{0});

  std::println("file  {} ({} bytes)", path, with_commas(buf.size()));

  std::println("\nsystem events");
  for (const auto &[code, ts] : h.system_events)
    std::println("  {}  {}  {}", code, format_ts(ts), event_name(code));

  std::println("\nmessage types");
  for (std::size_t i = 0; i < h.counts.size(); ++i)
    if (h.counts[i] != 0)
      std::println("  {}  {:>15}  {}", static_cast<char>(i),
                   with_commas(h.counts[i]), type_name(static_cast<char>(i)));

  const bool consumed_ok = res.bytes_consumed == buf.size();
  const bool counted_ok = counted == res.messages;
  std::println("\nchecks");
  std::println("  consumed        {} / {}  {}",
               with_commas(res.bytes_consumed), with_commas(buf.size()),
               consumed_ok ? "ok" : "MISMATCH");
  std::println("  bad_length      {}  {}", res.bad_length,
               res.bad_length == 0 ? "ok" : "FAIL");
  std::println("  ts_regressions  {}  {}", h.ts_regressions,
               h.ts_regressions == 0 ? "ok" : "FAIL");
  std::println("  counted         {} / {}  {}", with_commas(counted),
               with_commas(res.messages), counted_ok ? "ok" : "MISMATCH");

  if (res.messages > 0 && secs > 0) {
    const double msgs = static_cast<double>(res.messages);
    std::println("\nspeed (decode + count)");
    std::println("  time         {:.3f} s", secs);
    std::println("  throughput   {:.1f} M msg/s", msgs / secs / 1e6);
    std::println("  per message  {:.2f} ns", secs * 1e9 / msgs);
    std::println("  bandwidth    {:.2f} GB/s",
                 static_cast<double>(buf.size()) / secs / 1e9);
  }

  const bool ok = consumed_ok && counted_ok && res.bad_length == 0 &&
                  h.ts_regressions == 0;
  std::println("\n{}", ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}

int run_book(const char *path) {
  const itch::MappedFile file(path);
  const auto buf = file.bytes();
  CheckedBook book({"AAPL", "MSFT", "AMZN", "TSLA", "SPY", "QQQ"});

  const auto t0 = std::chrono::steady_clock::now();
  const auto res = itch::decode(buf, book);
  const auto t1 = std::chrono::steady_clock::now();
  const bool valid = book.validate(); // slow full rebuild, so not timed
  const auto t2 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  const double vsecs = std::chrono::duration<double>(t2 - t1).count();

  const auto &st = book.stats();
  std::println("file  {} ({} bytes)", path, with_commas(buf.size()));

  std::println("\nsystem events");
  for (const auto &[code, ts] : book.system_events)
    std::println("  {}  {}  {}", code, format_ts(ts), event_name(code));

  std::println("\norders");
  std::println("  peak live orders   {:>12}", with_commas(st.peak_orders));
  std::println("  open at end        {:>12}", with_commas(book.open_orders()));

  std::println("\ntop of book at close (16:00)");
  for (const auto &s : book.at_close) {
    auto side = [](const std::optional<itch::Quote> &q) {
      return q ? std::format("{:>9} x {:<7}", format_price(q->price),
                             with_commas(q->shares))
               : std::string("        -          ");
    };
    std::println("  {:<5} bid {}  ask {}", s.symbol, side(s.bid), side(s.ask));
  }

  const bool decode_ok =
      res.bytes_consumed == buf.size() && res.bad_length == 0;
  auto mark = [](bool ok) { return ok ? "ok" : "FAIL"; };
  std::println("\nchecks");
  std::println("  decode           {} msgs, bad_length {}  {}",
               with_commas(res.messages), res.bad_length, mark(decode_ok));
  std::println("  missing_refs     {}  {}", st.missing_refs,
               mark(st.missing_refs == 0));
  std::println("  duplicate_refs   {}  {}", st.duplicate_refs,
               mark(st.duplicate_refs == 0));
  std::println("  overfills        {}  {}", st.overfills,
               mark(st.overfills == 0));
  std::println("  validate()       {}  ({:.2f} s)", mark(valid), vsecs);
  std::println("  locked/crossed   {} / {}  (info)", with_commas(book.locked),
               with_commas(book.crossed));

  if (res.messages > 0 && secs > 0) {
    const double msgs = static_cast<double>(res.messages);
    std::println("\nspeed (decode + book)");
    std::println("  time         {:.3f} s", secs);
    std::println("  throughput   {:.1f} M msg/s", msgs / secs / 1e6);
    std::println("  per message  {:.2f} ns", secs * 1e9 / msgs);
  }

  const bool ok = decode_ok && valid && st.missing_refs == 0 &&
                  st.duplicate_refs == 0 && st.overfills == 0;
  std::println("\n{}", ok ? "OK" : "FAILED");
  return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  const std::string_view mode = argc == 3 ? argv[1] : "";
  if (!(argc == 2 || (argc == 3 && mode == "--book"))) {
    std::println(stderr, "usage: {} [--book] <itch50-file>", argv[0]);
    return 2;
  }
  try {
    return argc == 3 ? run_book(argv[2]) : run_count(argv[1]);
  } catch (const std::system_error &e) {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }
}
