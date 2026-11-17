// itch_export: replay a file through OrderBook and write CSVs for analysis
// (tools/plot.py).
//
//   <out>/book_<SYMBOL>.csv  top of book and depth, sampled every --interval-ms
//                            of exchange time, one file per symbol
//   <out>/activity.csv       messages per minute by type, all symbols
//   <out>/events.csv         system events (market open/close times, ...)
//
// A sample at time t shows the book after every message with timestamp < t.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "itch/book.hpp"
#include "itch/decoder.hpp"
#include "itch/mapped_file.hpp"

namespace {

namespace fs = std::filesystem;
using File = std::unique_ptr<std::FILE, int (*)(std::FILE *)>;

File open_out(const fs::path &p) {
  File f(std::fopen(p.c_str(), "w"), &std::fclose);
  if (!f)
    throw std::system_error(errno, std::generic_category(), p.string());
  return f;
}

std::string format_price(std::uint32_t p) {
  return std::format("{}.{:04}", p / 10'000, p % 10'000);
}

// ITCH alpha fields are left-justified and padded with spaces.
std::array<char, 8> pad(std::string_view s) {
  std::array<char, 8> a;
  a.fill(' ');
  for (std::size_t i = 0; i < s.size() && i < a.size(); ++i)
    a[i] = s[i];
  return a;
}

// Total shares in the best `n` levels (best price is at the back).
std::uint64_t depth(const itch::Levels &lv, std::size_t n) {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < n && i < lv.size(); ++i)
    sum += lv[lv.size() - 1 - i].shares;
  return sum;
}

class Exporter : public itch::OrderBook {
public:
  static constexpr std::size_t kDepthLevels = 5;

  Exporter(const std::vector<std::string> &symbols, std::uint64_t interval_ns,
           const fs::path &dir)
      : interval_(interval_ns), dir_(dir), minutes_(24 * 60) {
    for (const auto &s : symbols) {
      Watched w{s, pad(s), std::nullopt, open_out(dir / ("book_" + s + ".csv"))};
      std::println(w.out.get(), "time_ns,bid,bid_size,ask,ask_size,"
                                "bid_depth5,ask_depth5,bid_levels,ask_levels");
      watched_.push_back(std::move(w));
    }
  }

  // Every handler first takes samples up to this message's timestamp, then
  // lets OrderBook apply the message.
  void on_system_event(const itch::SystemEvent &m) {
    tick(m.hdr.timestamp);
    events_.emplace_back(m.event_code, m.hdr.timestamp);
  }
  void on_stock_directory(const itch::StockDirectory &m) {
    tick(m.hdr.timestamp);
    OrderBook::on_stock_directory(m);
    for (auto &w : watched_)
      if (w.key == m.stock)
        w.locate = m.hdr.locate;
  }
  void on_add(const itch::AddOrder &m) {
    seen(m.hdr, kAdd);
    OrderBook::on_add(m);
  }
  void on_order_executed(const itch::OrderExecuted &m) {
    seen(m.hdr, kExec);
    OrderBook::on_order_executed(m);
  }
  void on_order_executed_with_price(const itch::OrderExecutedWithPrice &m) {
    seen(m.hdr, kExec);
    OrderBook::on_order_executed_with_price(m);
  }
  void on_order_cancel(const itch::OrderCancel &m) {
    seen(m.hdr, kCancel);
    OrderBook::on_order_cancel(m);
  }
  void on_order_delete(const itch::OrderDelete &m) {
    seen(m.hdr, kDelete);
    OrderBook::on_order_delete(m);
  }
  void on_order_replace(const itch::OrderReplace &m) {
    seen(m.hdr, kReplace);
    OrderBook::on_order_replace(m);
  }

  // Writes activity.csv and events.csv; reports per-symbol row counts.
  void finish() {
    File act = open_out(dir_ / "activity.csv");
    std::println(act.get(), "minute,adds,executions,cancels,deletes,replaces");
    for (std::size_t i = 0; i < minutes_.size(); ++i) {
      const auto &c = minutes_[i];
      if (c[kAdd] + c[kExec] + c[kCancel] + c[kDelete] + c[kReplace] == 0)
        continue;
      std::println(act.get(), "{},{},{},{},{},{}", i, c[kAdd], c[kExec],
                   c[kCancel], c[kDelete], c[kReplace]);
    }
    File ev = open_out(dir_ / "events.csv");
    std::println(ev.get(), "code,time_ns");
    for (const auto &[code, ts] : events_)
      std::println(ev.get(), "{},{}", code, ts);

    for (const auto &w : watched_)
      std::println("  book_{}.csv  {} rows{}", w.name, w.rows,
                   w.locate ? "" : "  (symbol not in this file)");
  }

private:
  enum Type { kAdd, kExec, kCancel, kDelete, kReplace, kTypes };

  struct Watched {
    std::string name;
    std::array<char, 8> key;
    std::optional<std::uint16_t> locate; // known once its 'R' message arrives
    File out;
    std::uint64_t rows = 0;
  };

  void seen(const itch::Header &h, Type t) {
    tick(h.timestamp);
    const std::size_t minute = h.timestamp / 60'000'000'000ULL;
    if (minute < minutes_.size())
      ++minutes_[minute][t];
  }

  // Emit a sample for every interval boundary at or before `ts`.
  void tick(std::uint64_t ts) {
    if (next_ == 0)
      next_ = (ts / interval_ + 1) * interval_;
    while (next_ <= ts) {
      sample(next_);
      next_ += interval_;
    }
  }

  void sample(std::uint64_t t) {
    for (auto &w : watched_) {
      if (!w.locate)
        continue;
      const itch::Book &b = book(*w.locate);
      const auto bid = best_bid(*w.locate);
      const auto ask = best_ask(*w.locate);
      std::println(w.out.get(), "{},{},{},{},{},{},{},{},{}", t,
                   bid ? format_price(bid->price) : "",
                   bid ? std::to_string(bid->shares) : "",
                   ask ? format_price(ask->price) : "",
                   ask ? std::to_string(ask->shares) : "",
                   depth(b.bids, kDepthLevels), depth(b.asks, kDepthLevels),
                   b.bids.size(), b.asks.size());
      ++w.rows;
    }
  }

  std::uint64_t interval_;
  std::uint64_t next_ = 0;
  fs::path dir_;
  std::vector<Watched> watched_;
  std::vector<std::array<std::uint64_t, kTypes>> minutes_; // by minute of day
  std::vector<std::pair<char, std::uint64_t>> events_;
};

[[noreturn]] void usage(const char *argv0) {
  std::println(stderr,
               "usage: {} [--interval-ms N] [--out DIR] <itch50-file> SYMBOL...\n"
               "  --interval-ms N  sample period in exchange time (default 1000)\n"
               "  --out DIR        output directory (default out)",
               argv0);
  std::exit(2);
}

} // namespace

int main(int argc, char **argv) {
  std::uint64_t interval_ms = 1000;
  fs::path out = "out";
  const char *path = nullptr;
  std::vector<std::string> symbols;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if ((a == "--interval-ms" || a == "--out") && i + 1 < argc) {
      if (a == "--out")
        out = argv[++i];
      else
        interval_ms = std::stoull(argv[++i]);
    } else if (a.starts_with("--")) {
      usage(argv[0]);
    } else if (!path) {
      path = argv[i];
    } else if (a.size() <= 8) {
      symbols.emplace_back(a);
    } else {
      usage(argv[0]);
    }
  }
  if (!path || symbols.empty() || interval_ms == 0)
    usage(argv[0]);

  try {
    fs::create_directories(out);
    const itch::MappedFile file(path);
    auto ex = std::make_unique<Exporter>(symbols, interval_ms * 1'000'000, out);
    const auto res = itch::decode(file.bytes(), *ex);

    std::println("{}: {} messages -> {}/", path, res.messages, out.string());
    ex->finish();
    std::println("  activity.csv, events.csv");

    const auto &st = ex->stats();
    if (st.missing_refs || st.duplicate_refs || st.overfills) {
      std::println(stderr, "error: book inconsistent (missing={} duplicate={} overfill={})",
                   st.missing_refs, st.duplicate_refs, st.overfills);
      return 1;
    }
    return 0;
  } catch (const std::exception &e) {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }
}
