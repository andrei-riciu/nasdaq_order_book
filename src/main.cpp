#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <numeric>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::println(stderr, "usage: {} <itch50-file>", argv[0]);
    return 2;
  }
  try {
    const itch::MappedFile file(argv[1]);
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

    std::println("file  {} ({} bytes)", argv[1], with_commas(buf.size()));

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
  } catch (const std::system_error &e) {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }
}
