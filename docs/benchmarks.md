# Benchmarks

All numbers come from `itch_bench` (`bench/itch_bench.cpp`) on the setup below.
Numbers are only compared when they come from the same input and the same
method.

## Setup

| | |
|---|---|
| CPU | Intel Core Ultra 7 255H (6 P + 8 E + 2 LP-E cores), 24 MB L3 |
| Pinned to | CPU 2 (P-core, up to 5.1 GHz) via `taskset -c 2` |
| RAM | 32 GB (laptop, on AC power) |
| OS / kernel | Fedora 43, Linux 7.2 |
| Compiler | GCC 15.3.1, `-O3 -march=native` (CMake Release) |

## Method

- **Input in RAM.** The file (or its first N GB) is `read()` into a heap
  buffer before any timing. Disk, the page cache and page faults are never
  inside the timed region. Every run reports its **major page faults**, which
  must be 0.
- **Only `decode()` is timed.** Building the book before a run and freeing it
  after are not timed.
- **Repeated.** 1 warmup run, then N timed runs, each on a fresh book. Reported
  as the median, with the min and max.
- **Two modes.**
  - *decode only*: parse every message and sum its fields (a checksum), so
    the compiler cannot delete the parsing.
  - *decode + book*: the same messages applied to `OrderBook`.

  *Book alone* is the difference between the two medians.
- **Hardware counters.** `perf_event_open` on the P-core PMU, user space only,
  read around `decode()` only.
- **Latency** (`--latency`). This is a separate run and is never used for
  throughput. Each book update is timed with `rdtsc` … `rdtscp` into a
  histogram. Decoding is not included. The timer itself costs about 8 ns.
- **Correctness gate.** A book run aborts if any missing, duplicate or
  overfilled order ref appears. Every optimization must also keep
  `itch_replay --book` at `OK` on both files.

```sh
taskset -c 2 build/itch_bench data/20190730.BX_ITCH_50
taskset -c 2 build/itch_bench --gb 4 --runs 3 [--latency] data/01302020.NASDAQ_ITCH50
```

### Inputs

| Name | File | Messages | Covers | Peak live orders |
|---|---|---|---|---|
| **BX** | `20190730.BX_ITCH_50`, whole file | 28.7 M | 03:06 – 19:05 | 67 k |
| **NQ-4G** | `01302020.NASDAQ_ITCH50`, first 4 GB | 130.4 M | 03:02 – 11:02, including the open | ~1.9 M |

NQ-4G is the main benchmark. The whole Nasdaq file (12.9 GB) only just fits
next to everything else in RAM and starts swapping, so it is only used for
end-to-end runs. It is also a different workload: it ran at about 230 ns/msg
against 217 ns/msg for NQ-4G.

## Baseline (v0: `std::unordered_map` orders + `std::map` levels)

Commit `2398acf`.

### Throughput

| Input | Decode only | Decode + book | Book alone | M msg/s (book) |
|---|---|---|---|---|
| BX | 7.3 ns | 55.2 ns | 47.9 ns | 18.1 |
| NQ-4G | 6.9 ns | **216.8 ns** | **209.9 ns** | **4.6** |

Run-to-run spread is under 0.5%.

### Hardware counters, per message (decode + book)

| Input | Cycles | Instructions | IPC | LLC misses | dTLB misses | Branch misses |
|---|---|---|---|---|---|---|
| BX | 280 | 312 | 1.12 | 0.45 | 0.04 | 3.3 |
| NQ-4G | 1,100 | 361 | **0.33** | **9.4** | **4.7** | 4.7 |

For comparison, decode alone on NQ-4G: 35 cycles, 41 instructions, IPC 1.15,
0.26 LLC misses per message.

### Book update latency, NQ-4G (ns)

| Type | Count | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| A (add) | 57.5 M | 165 | 495 | 914 | 1,305 | **132.1 ms** |
| E (executed) | 2.3 M | 219 | 737 | 1,495 | 1,853 | 46.0 µs |
| C (executed w/ price) | 43 k | 86 | 446 | 1,293 | 1,675 | 5.2 µs |
| X (cancel) | 2.6 M | 55 | 332 | 854 | 1,436 | 54.1 µs |
| D (delete) | 54.1 M | 180 | 689 | 1,364 | 1,771 | 62.2 µs |
| U (replace) | 11.9 M | 385 | 1,074 | 1,547 | 1,948 | 530.0 µs |
| all | 128.5 M | 182 | 627 | 1,316 | 1,721 | 132.1 ms |

### Where the time goes (NQ-4G, `perf record`)

By function (cycles), with the hash table and tree code inlined into its caller:

| Function | Share |
|---|---|
| `OrderBook::add` | 35% |
| `OrderBook::reduce` | 33% |
| `OrderBook::on_order_delete` | 14% (its own `find` before `reduce`) |
| `on_order_replace` | 3% |
| `decode` loop | 2% |
| `malloc` / `free` | ~2% |

By data structure (samples of loads that missed L3, `mem_load_retired.l3_miss`):

| Structure | Share of L3-miss loads | Where |
|---|---|---|
| `std::unordered_map` (orders) | **~54%** | bucket array (~6%), walking to the node and comparing its key (~29%), touching a neighbouring node's key to fix bucket pointers on insert and erase (~16%) |
| `std::map` (price levels) | **~42%** | tree descent in `lower_bound`, child-pointer loads |
| everything else | ~4% | |

## Analysis

- **Decoding is not the bottleneck.** It runs at about 7 ns/msg (140 M msg/s,
  IPC ~1.1) and is about 3% of the decode + book time on NQ-4G.
- **The book is memory-latency bound.** It executes about the same number of
  instructions per message on both inputs (312 vs 361). On NQ-4G, cycles grow
  4× and IPC falls to 0.33, with about 9.4 LLC misses and 4.7 dTLB misses per
  message. BX's 67 k live orders fit in cache; NQ's ~1.9 M do not.
- **Both containers are node-based**, and each node is a separate heap
  allocation:
  - an order lookup is a bucket load, then a node load, and insert and erase
    also touch a neighbouring node;
  - a level lookup is a pointer chase down a red-black tree.
- **Tail latency.** The 132 ms worst-case add was an `unordered_map` rehash:
  about 2 M nodes reinserted inside one `on_add`. v1 confirmed this: with the
  table presized, the worst case fell to 1.2 ms and the worst-case replace
  fell from 530 µs to 74 µs. The 1.2 ms that remains does not come from a
  rehash, because none can happen any more; it is unexplained so far.
- **Duplicate lookup.** `on_order_delete` and `on_order_replace` call `find`,
  then `reduce` calls `find` again on the same ref.

## Plan (each step measured against this baseline)

1. **Orders:** an open-addressing hash table. It is flat, presized to a power
   of two, uses linear probing and backward-shift deletion, and does no
   allocation or rehash during a run. This targets about 54% of the L3 misses
   and the 132 ms tail.
2. **Levels:** a sorted `std::vector` per side, best price at the back. This
   targets about 42% of the misses.
3. **Huge pages** for the order table, against the 4.7 dTLB misses per
   message.
4. **Remove the double lookup** in delete and replace.

## History

| Version | Change | NQ-4G decode + book | LLC miss/msg | p99 | max |
|---|---|---|---|---|---|
| v0 | baseline | 216.8 ns | 9.4 | 1,316 ns | 132.1 ms |
| v1 | `orders_.reserve(4M)`: no rehash during a run | 195.7 ns (−9.7%) | 8.5 | 1,250 ns | 1.2 ms |
