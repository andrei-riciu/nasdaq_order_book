// itch_bench: repeatable throughput / latency measurements.
//
// Differences from itch_replay:
//   * the input is read into RAM *before* any timing, so disk and page faults
//     are never inside the timed region;
//   * every mode runs warmup + N times on a fresh handler, and reports
//     median / min / max instead of a single number;
//   * setup (constructing the book) and teardown (freeing it) are outside the
//     timer;
//   * hardware counters (cycles, cache/TLB misses...) are read only around
//     the timed decode(), and major page faults are checked to be zero;
//   * optional per-message latency histograms for the book update.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

#include "itch/book.hpp"
#include "itch/decoder.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Buf = std::span<const std::uint8_t>;

// ---------------------------------------------------------------------------
// Input

struct Options {
  const char *path = nullptr;
  double gb = 0; // 0 = whole file
  int runs = 5;
  int warmup = 1;
  bool latency = false;
  bool run_decode = true;
  bool run_book = true;
};

[[noreturn]] void usage(const char *argv0) {
  std::println(stderr,
               "usage: {} [--gb N] [--runs N] [--warmup N] [--mode M] [--latency] <file>\n"
               "  --gb N      only the first N GB of the file (default: all)\n"
               "  --runs N    timed runs per mode (default 5)\n"
               "  --warmup N  untimed runs first (default 1)\n"
               "  --mode M    decode | book | both (default both)\n"
               "  --latency   also measure per-message book update latency",
               argv0);
  std::exit(2);
}

Options parse_args(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto next = [&] {
      if (i + 1 >= argc)
        usage(argv[0]);
      return argv[++i];
    };
    if (a == "--gb")
      o.gb = std::stod(next());
    else if (a == "--runs")
      o.runs = std::stoi(next());
    else if (a == "--warmup")
      o.warmup = std::stoi(next());
    else if (a == "--latency")
      o.latency = true;
    else if (a == "--mode") {
      const std::string_view m = next();
      if (m != "decode" && m != "book" && m != "both")
        usage(argv[0]);
      o.run_decode = m != "book";
      o.run_book = m != "decode";
    }
    else if (!a.starts_with("--") && !o.path)
      o.path = argv[i];
    else
      usage(argv[0]);
  }
  if (!o.path || o.runs < 1 || o.warmup < 0 || o.gb < 0)
    usage(argv[0]);
  return o;
}

// Read up to `limit` bytes (0 = all) into ordinary heap memory. Unlike the
// page cache behind an mmap, the kernel cannot quietly drop this between runs.
std::vector<std::uint8_t> load(const char *path, std::size_t limit) {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(), path);
  const off_t end = ::lseek(fd, 0, SEEK_END);
  ::lseek(fd, 0, SEEK_SET);
  std::size_t n = static_cast<std::size_t>(end);
  if (limit != 0)
    n = std::min(n, limit);

  std::vector<std::uint8_t> v(n); // zero-filling also prefaults every page
  for (std::size_t got = 0; got < n;) {
    const ssize_t r = ::read(fd, v.data() + got, n - got);
    if (r <= 0) {
      const int err = r < 0 ? errno : EIO;
      ::close(fd);
      throw std::system_error(err, std::generic_category(), path);
    }
    got += static_cast<std::size_t>(r);
  }
  ::close(fd);
  return v;
}

// Walk the frames: how many bytes are whole messages, and the timestamp of
// the last one (so we can say which part of the day a slice covers).
struct SliceInfo {
  std::size_t bytes = 0;
  std::uint64_t last_ts = 0;
};
SliceInfo whole_messages(Buf b) {
  SliceInfo s;
  std::size_t pos = 0;
  while (pos + 2 <= b.size()) {
    const std::size_t len = itch::detail::load_be<std::uint16_t>(b.data() + pos);
    if (pos + 2 + len > b.size())
      break;
    if (len >= 11)
      s.last_ts = itch::detail::load_be48(b.data() + pos + 2 + 5);
    pos += 2 + len;
  }
  s.bytes = pos;
  return s;
}

std::string format_ts(std::uint64_t ns) {
  const std::uint64_t s = ns / 1'000'000'000;
  return std::format("{:02}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60);
}

// ---------------------------------------------------------------------------
// Handlers

// "Decode only" must still *use* every parsed field: with an empty handler
// the optimizer is allowed to delete the parsing and we'd time nothing.
// Adding fields into a sum is about one instruction each.
struct ChecksumHandler : itch::NullHandler {
  std::uint64_t sum = 0;
  void on_system_event(const itch::SystemEvent &m) {
    sum += m.hdr.timestamp + static_cast<std::uint8_t>(m.event_code);
  }
  void on_stock_directory(const itch::StockDirectory &m) {
    sum += m.hdr.locate + static_cast<std::uint8_t>(m.stock[0]);
  }
  void on_add(const itch::AddOrder &m) {
    sum += m.hdr.timestamp + m.ref + m.shares + m.price +
           static_cast<std::uint8_t>(m.side) +
           static_cast<std::uint8_t>(m.stock[0]);
  }
  void on_order_executed(const itch::OrderExecuted &m) {
    sum += m.hdr.timestamp + m.ref + m.shares + m.match;
  }
  void on_order_executed_with_price(const itch::OrderExecutedWithPrice &m) {
    sum += m.hdr.timestamp + m.ref + m.shares + m.match + m.price;
  }
  void on_order_cancel(const itch::OrderCancel &m) {
    sum += m.hdr.timestamp + m.ref + m.shares;
  }
  void on_order_delete(const itch::OrderDelete &m) {
    sum += m.hdr.timestamp + m.ref;
  }
  void on_order_replace(const itch::OrderReplace &m) {
    sum += m.hdr.timestamp + m.old_ref + m.new_ref + m.shares + m.price;
  }
};

// Book updates timed one by one with the TSC (constant rate on this CPU,
// independent of turbo). Only the OrderBook call is inside the window, not
// the decoding. rdtscp at the end waits for the work before it to finish.
class TimedBook : public itch::OrderBook {
public:
  // 4-cycle buckets up to 32768 cycles (~10 us); anything slower is counted
  // in `over` and shows up in `max`.
  static constexpr std::size_t kBuckets = 8192;
  static constexpr std::uint64_t kWidth = 4;

  struct Hist {
    std::array<std::uint64_t, kBuckets> b{};
    std::uint64_t n = 0, over = 0, max = 0;
    void add(std::uint64_t cyc) {
      ++n;
      max = std::max(max, cyc);
      const std::uint64_t i = cyc / kWidth;
      if (i < kBuckets)
        ++b[i];
      else
        ++over;
    }
    void merge(const Hist &o) {
      for (std::size_t i = 0; i < kBuckets; ++i)
        b[i] += o.b[i];
      n += o.n;
      over += o.over;
      max = std::max(max, o.max);
    }
    // Upper edge of the bucket holding the p-th percentile, in cycles.
    // Returns 0 if it lies in the overflow region.
    std::uint64_t percentile(double p) const {
      const auto target = static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(n));
      std::uint64_t seen = 0;
      for (std::size_t i = 0; i < kBuckets; ++i) {
        seen += b[i];
        if (seen > target)
          return (i + 1) * kWidth;
      }
      return 0;
    }
  };

  static constexpr std::string_view kTypes = "AECXDU";
  std::array<Hist, 6> hist; // indexed like kTypes

  void on_add(const itch::AddOrder &m) {
    const auto t = __rdtsc();
    OrderBook::on_add(m);
    record(0, t);
  }
  void on_order_executed(const itch::OrderExecuted &m) {
    const auto t = __rdtsc();
    OrderBook::on_order_executed(m);
    record(1, t);
  }
  void on_order_executed_with_price(const itch::OrderExecutedWithPrice &m) {
    const auto t = __rdtsc();
    OrderBook::on_order_executed_with_price(m);
    record(2, t);
  }
  void on_order_cancel(const itch::OrderCancel &m) {
    const auto t = __rdtsc();
    OrderBook::on_order_cancel(m);
    record(3, t);
  }
  void on_order_delete(const itch::OrderDelete &m) {
    const auto t = __rdtsc();
    OrderBook::on_order_delete(m);
    record(4, t);
  }
  void on_order_replace(const itch::OrderReplace &m) {
    const auto t = __rdtsc();
    OrderBook::on_order_replace(m);
    record(5, t);
  }

private:
  void record(std::size_t type, std::uint64_t start) {
    unsigned aux;
    hist[type].add(__rdtscp(&aux) - start);
  }
};

// ---------------------------------------------------------------------------
// Hardware counters

// Counts events for *this* process in user space only, which needs no root
// (perf_event_paranoid <= 2). Started/stopped around decode() only, so loading
// the file, building and freeing the book are never counted.
class PerfCounters {
public:
  static constexpr std::size_t N = 5;
  using Values = std::array<std::uint64_t, N>;

  PerfCounters() {
    // Hybrid Intel CPUs have one PMU per core type. Ask for the P-core PMU
    // ("cpu_core"), where we pin; on other machines 0 means "the default".
    std::uint64_t pmu = 0;
    std::ifstream("/sys/bus/event_source/devices/cpu_core/type") >> pmu;

    constexpr std::uint64_t dtlb_read_miss =
        PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
        (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
    const std::array<std::pair<std::uint32_t, std::uint64_t>, N> events = {{
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES}, // last-level cache
        {PERF_TYPE_HW_CACHE, dtlb_read_miss},
        {PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
    }};

    for (std::size_t i = 0; i < N; ++i) {
      perf_event_attr a{};
      a.size = sizeof a;
      a.type = events[i].first;
      a.config = events[i].second | (pmu << 32);
      a.disabled = i == 0 ? 1 : 0; // the group leader switches all of them
      a.exclude_kernel = 1;
      a.exclude_hv = 1;
      a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                      PERF_FORMAT_TOTAL_TIME_RUNNING;
      const long fd = ::syscall(SYS_perf_event_open, &a, 0 /*this process*/,
                                -1 /*any cpu*/, i == 0 ? -1 : fds_[0], 0);
      if (fd < 0) {
        close_all();
        return;
      }
      fds_[i] = static_cast<int>(fd);
    }
  }
  ~PerfCounters() { close_all(); }
  PerfCounters(const PerfCounters &) = delete;
  PerfCounters &operator=(const PerfCounters &) = delete;

  bool ok() const { return fds_[0] >= 0; }

  void start() {
    if (!ok())
      return;
    ::ioctl(fds_[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ::ioctl(fds_[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
  }

  Values stop() {
    Values v{};
    if (!ok())
      return v;
    ::ioctl(fds_[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    struct {
      std::uint64_t nr, enabled, running, value[N];
    } r{};
    if (::read(fds_[0], &r, sizeof r) != static_cast<ssize_t>(sizeof r) ||
        r.running == 0)
      return v;
    // If the kernel had to share the counters with someone else, the group
    // only ran part of the time: scale up (perf stat does the same).
    const double scale = static_cast<double>(r.enabled) / static_cast<double>(r.running);
    for (std::size_t i = 0; i < N; ++i)
      v[i] = static_cast<std::uint64_t>(static_cast<double>(r.value[i]) * scale);
    return v;
  }

private:
  void close_all() {
    for (int &fd : fds_)
      if (fd >= 0) {
        ::close(fd);
        fd = -1;
      }
  }
  std::array<int, N> fds_{-1, -1, -1, -1, -1};
};

// Page faults that had to wait for disk or swap. With the input in RAM this
// must stay 0 during timed runs, or the run was disturbed (e.g. swapping).
long major_faults() {
  rusage u{};
  ::getrusage(RUSAGE_SELF, &u);
  return u.ru_majflt;
}

// ---------------------------------------------------------------------------
// Running and reporting

// A book replay must stay consistent, or its timing means nothing.
void require_clean(const itch::OrderBook &b) {
  const auto &s = b.stats();
  if (s.missing_refs || s.duplicate_refs || s.overfills)
    throw std::runtime_error(std::format(
        "book inconsistent: missing={} duplicate={} overfill={}",
        s.missing_refs, s.duplicate_refs, s.overfills));
}

struct Timing {
  std::vector<double> ns_per_msg; // one per timed run
  std::size_t messages = 0;       // per run
  PerfCounters::Values counters{}; // summed over timed runs
  long major_faults = 0;           // summed over timed runs
};

// Runs `warmup + runs` replays, each on a fresh handler. Construction and
// destruction of the handler happen outside the timed region.
template <class Handler, class Check>
Timing time_mode(std::string_view name, Buf buf, const Options &o,
                 PerfCounters &pc, Check check) {
  Timing t;
  for (int i = 0; i < o.warmup + o.runs; ++i) {
    auto h = std::make_unique<Handler>();
    const long f0 = major_faults();
    pc.start();
    const auto t0 = Clock::now();
    const auto r = itch::decode(buf, *h);
    const auto t1 = Clock::now();
    const auto counts = pc.stop();
    const long f1 = major_faults();
    check(*h);
    t.messages = r.messages;
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() /
                      static_cast<double>(r.messages);
    const bool warm = i < o.warmup;
    if (!warm) {
      t.ns_per_msg.push_back(ns);
      for (std::size_t k = 0; k < counts.size(); ++k)
        t.counters[k] += counts[k];
      t.major_faults += f1 - f0;
    }
    std::println(stderr, "  {:<7} run {}/{}  {:7.2f} ns/msg{}", name,
                 i + 1, o.warmup + o.runs, ns, warm ? "  (warmup)" : "");
  } // handler freed here, outside the timer
  return t;
}

double median(std::vector<double> v) {
  std::ranges::sort(v);
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

void print_row(std::string_view name, const Timing &t) {
  const auto [lo, hi] = std::ranges::minmax(t.ns_per_msg);
  const double med = median(t.ns_per_msg);
  std::println("  {:<16} {:8.2f} {:8.2f} {:8.2f} {:10.1f}", name, med, lo, hi,
               1e3 / med);
}

void print_counters(std::string_view name, const Timing &t) {
  const double msgs =
      static_cast<double>(t.messages) * static_cast<double>(t.ns_per_msg.size());
  auto per = [&](std::size_t i) { return static_cast<double>(t.counters[i]) / msgs; };
  std::println("  {:<16} {:8.1f} {:8.1f} {:6.2f} {:9.3f} {:9.3f} {:8.3f}", name,
               per(0), per(1), per(1) / per(0), per(2), per(3), per(4));
}

// TSC ticks per nanosecond, measured against steady_clock.
double tsc_per_ns() {
  const auto c0 = Clock::now();
  const auto t0 = __rdtsc();
  while (Clock::now() - c0 < std::chrono::milliseconds(200)) {
  }
  const auto t1 = __rdtsc();
  const auto c1 = Clock::now();
  return static_cast<double>(t1 - t0) /
         std::chrono::duration<double, std::nano>(c1 - c0).count();
}

// Cost of the timing itself (rdtsc ... rdtscp with nothing in between).
TimedBook::Hist timer_overhead() {
  TimedBook::Hist h;
  for (int i = 0; i < 1'000'000; ++i) {
    const auto t = __rdtsc();
    unsigned aux;
    h.add(__rdtscp(&aux) - t);
  }
  return h;
}

void print_latency(const TimedBook &b, double ticks_per_ns) {
  auto ns = [&](std::uint64_t cyc) -> std::string {
    if (cyc == 0)
      return std::format("{:>8}", ">10us");
    return std::format("{:8.0f}", static_cast<double>(cyc) / ticks_per_ns);
  };
  auto row = [&](std::string_view name, const TimedBook::Hist &h) {
    if (h.n == 0)
      return;
    std::println("  {:<6} {:>13} {} {} {} {} {}", name, h.n,
                 ns(h.percentile(50)), ns(h.percentile(90)),
                 ns(h.percentile(99)), ns(h.percentile(99.9)), ns(h.max));
  };

  std::println("\nbook update latency, ns (TSC {:.3f} GHz; decode not included)",
               ticks_per_ns);
  std::println("  {:<6} {:>13} {:>8} {:>8} {:>8} {:>8} {:>8}", "type",
               "count", "p50", "p90", "p99", "p99.9", "max");
  TimedBook::Hist all;
  for (std::size_t i = 0; i < b.hist.size(); ++i) {
    row(std::string(1, TimedBook::kTypes[i]), b.hist[i]);
    all.merge(b.hist[i]);
  }
  row("all", all);
  row("timer", timer_overhead());
}

} // namespace

int main(int argc, char **argv) {
  const Options o = parse_args(argc, argv);
  try {
    // Pinning check: an unpinned run can land on an LP-E core and be ~7x
    // slower, which would make the numbers meaningless.
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);
    const int allowed = CPU_COUNT(&set);
    if (allowed != 1)
      std::println(stderr,
                   "warning: not pinned to one CPU ({} allowed); "
                   "run with: taskset -c 2 {} ...",
                   allowed, argv[0]);

    const auto limit = static_cast<std::size_t>(o.gb * 1e9);
    std::println(stderr, "loading {} into RAM...", o.path);
    const auto data = load(o.path, limit);
    const SliceInfo slice = whole_messages(data);
    const Buf buf(data.data(), slice.bytes);

    std::println("file     {}", o.path);
    std::println("input    {:.2f} GB in RAM, up to {}{}",
                 static_cast<double>(slice.bytes) / 1e9, format_ts(slice.last_ts),
                 limit ? " (slice)" : " (whole file)");
    std::println("cpu      {} (allowed: {})", sched_getcpu(), allowed);
    std::println("runs     {} timed + {} warmup per mode", o.runs, o.warmup);

    PerfCounters pc;
    std::uint64_t checksum = 0;
    std::optional<Timing> dec, book;
    if (o.run_decode)
      dec = time_mode<ChecksumHandler>(
          "decode", buf, o, pc, [&](const ChecksumHandler &h) { checksum = h.sum; });
    if (o.run_book)
      book = time_mode<itch::OrderBook>("book", buf, o, pc, require_clean);

    std::println("\nmessages {}", (dec ? dec : book)->messages);
    std::println("\nthroughput           ns/msg   (median      min      max)   M msg/s");
    if (dec)
      print_row("decode only", *dec);
    if (book)
      print_row("decode + book", *book);
    if (dec && book)
      std::println("  {:<16} {:8.2f}   (median difference)", "book alone",
                   median(book->ns_per_msg) - median(dec->ns_per_msg));

    if (pc.ok()) {
      std::println("\nper message          cycles    instr    IPC  LLC-miss dTLB-miss  br-miss");
      if (dec)
        print_counters("decode only", *dec);
      if (book)
        print_counters("decode + book", *book);
    } else {
      std::println("\n(hardware counters unavailable: check perf_event_paranoid)");
    }

    const long faults = (dec ? dec->major_faults : 0) + (book ? book->major_faults : 0);
    std::println("\nmajor page faults in timed runs: {}  {}", faults,
                 faults == 0 ? "ok" : "WARNING: disk/swap inside timing, rerun");

    if (o.latency) {
      // Separate run: the timing calls themselves slow things down, so this
      // run is never used for the throughput numbers above.
      std::println(stderr, "  latency run...");
      auto tb = std::make_unique<TimedBook>();
      itch::decode(buf, *tb);
      require_clean(*tb);
      print_latency(*tb, tsc_per_ns());
    }

    std::println("\n(checksum {:#x})", checksum); // keeps decode-only honest
    return 0;
  } catch (const std::exception &e) {
    std::println(stderr, "error: {}", e.what());
    return 1;
  }
}
