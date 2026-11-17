#!/usr/bin/env python3
"""Charts from itch_export output and docs/benchmarks.md.

    python3 tools/plot.py book     out/book_AAPL.csv   # price, spread, depth
    python3 tools/plot.py activity out/activity.csv    # messages per minute
    python3 tools/plot.py bench    docs/benchmarks.md  # optimization history
    python3 tools/plot.py all      out/                # all of the above

PNGs go to docs/img/ unless --out says otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # files only, no display needed (also works in CI)

import matplotlib.dates as mdates  # noqa: E402
import matplotlib.ticker  # noqa: E402,F401
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

# ---------------------------------------------------------------------------
# Style: one validated categorical order, recessive axes and grid.

SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_2 = "#52514e"
GRID = "#e4e3df"
# Categorical slots in fixed order (never cycled). Identity is stable across
# charts: bid is always slot 1, ask always slot 2.
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
BID, ASK = SERIES[0], SERIES[1]
NEUTRAL = INK_2  # single derived series (spread)

plt.rcParams.update({
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK_2,
    "axes.titlecolor": INK,
    "axes.titlesize": 11,
    "axes.titleweight": "bold",
    "axes.titlelocation": "left",
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.grid": True,
    "axes.grid.axis": "y",
    "axes.axisbelow": True,  # grid behind bars and lines
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "xtick.color": INK_2,
    "ytick.color": INK_2,
    "text.color": INK,
    "legend.frameon": False,
    "lines.linewidth": 1.2,
    "font.size": 9,
})

DAY = pd.Timestamp("2000-01-01")  # ITCH times are ns since midnight; date is irrelevant
OPEN_NS = 9 * 3600 * 10**9 + 30 * 60 * 10**9
CLOSE_NS = 16 * 3600 * 10**9


def to_time(ns: pd.Series) -> pd.Series:
    return DAY + pd.to_timedelta(ns, unit="ns")


def hhmm_axis(ax) -> None:
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))


def market_hours(directory: Path) -> tuple[int, int]:
    """Open/close from events.csv next to the data, else 09:30 / 16:00."""
    events = directory / "events.csv"
    if events.exists():
        ev = pd.read_csv(events)
        q = ev.loc[ev["code"] == "Q", "time_ns"]
        m = ev.loc[ev["code"] == "M", "time_ns"]
        if len(q) and len(m):
            return int(q.iloc[0]), int(m.iloc[0])
    return OPEN_NS, CLOSE_NS


def save(fig, out: Path, name: str) -> Path:
    out.mkdir(parents=True, exist_ok=True)
    path = out / name
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  wrote {path}")
    return path


# ---------------------------------------------------------------------------
# book_<SYMBOL>.csv -> price, spread, depth


def plot_book(csv: Path, out: Path, all_day: bool = False) -> Path:
    symbol = csv.stem.removeprefix("book_")
    df = pd.read_csv(csv)
    if not all_day:
        open_ns, close_ns = market_hours(csv.parent)
        df = df[(df["time_ns"] >= open_ns) & (df["time_ns"] <= close_ns)]
    df = df.dropna(subset=["bid", "ask"])  # one-sided or empty book: no spread
    if df.empty:
        raise ValueError(f"{csv}: no two-sided quotes in the selected hours")

    df = df.assign(spread=(df["ask"] - df["bid"]) * 100)  # cents
    # Raw samples are dominated by momentary gaps (the best order leaves and
    # the next level is far away for a second). Per-minute medians show the
    # day; the band keeps the spread's variability visible.
    per_min = df.set_index(to_time(df["time_ns"])).resample("1min")
    med = per_min[["bid", "ask", "spread", "bid_depth5", "ask_depth5"]].median().dropna()
    lo = per_min["spread"].quantile(0.10).reindex(med.index)
    hi = per_min["spread"].quantile(0.90).reindex(med.index)
    t = med.index

    fig, (ax_px, ax_sp, ax_dp) = plt.subplots(
        3, 1, figsize=(10, 7.5), sharex=True, height_ratios=[2, 1, 1.4])
    fig.suptitle(f"{symbol}: reconstructed order book",
                 x=0.01, ha="left", fontweight="bold", fontsize=12)
    fig.text(0.01, 0.945, "1-minute medians of 1-second samples, market hours",
             ha="left", color=INK_2)

    ax_px.plot(t, med["bid"], color=BID, label="best bid")
    ax_px.plot(t, med["ask"], color=ASK, label="best ask")
    ax_px.set_title("Best bid and ask")
    ax_px.set_ylabel("price ($)")
    ax_px.legend(loc="lower right", bbox_to_anchor=(1, 1), ncols=2)

    ax_sp.fill_between(t, lo, hi, color=NEUTRAL, alpha=0.15, linewidth=0,
                       label="10-90% of samples")
    ax_sp.plot(t, med["spread"], color=NEUTRAL, label="median")
    ax_sp.set_title("Bid-ask spread")
    ax_sp.set_ylabel("cents")
    # Thin moments can push the band to dollars; scale to the typical range
    # so the median stays readable, and say when the top is cut off.
    top = max(1.0, hi.quantile(0.95) * 1.5, med["spread"].quantile(0.95) * 1.5)
    ax_sp.set_ylim(0, top)
    if hi.max() > top:
        ax_sp.text(0, 0.97, f"axis cut at {top:.0f}c; brief spikes up to {hi.max():.0f}c",
                   transform=ax_sp.transAxes, va="top", color=INK_2, fontsize=8)
    ax_sp.legend(loc="lower right", bbox_to_anchor=(1, 1), ncols=2)

    ax_dp.plot(t, med["bid_depth5"], color=BID, label="bid")
    ax_dp.plot(t, med["ask_depth5"], color=ASK, label="ask")
    ax_dp.set_title("Shares in the best 5 price levels")
    ax_dp.set_ylabel("shares")
    ax_dp.set_ylim(0, None)
    ax_dp.yaxis.set_major_formatter(matplotlib.ticker.StrMethodFormatter("{x:,.0f}"))
    ax_dp.legend(loc="lower right", bbox_to_anchor=(1, 1), ncols=2)
    hhmm_axis(ax_dp)

    fig.align_ylabels()
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    print(f"{symbol}: median spread {df['spread'].median():.1f} cents, "
          f"median best-5 depth bid {df['bid_depth5'].median():,.0f} / "
          f"ask {df['ask_depth5'].median():,.0f} shares ({len(df):,} samples)")
    return save(fig, out, f"{symbol}_book.png")


# ---------------------------------------------------------------------------
# activity.csv -> messages per minute by type

ACTIVITY = [  # (column, label) in fixed slot order
    ("adds", "add (A/F)"),
    ("deletes", "delete (D)"),
    ("replaces", "replace (U)"),
    ("executions", "execute (E/C)"),
    ("cancels", "cancel (X)"),
]


def plot_activity(csv: Path, out: Path) -> Path:
    df = pd.read_csv(csv)
    t = DAY + pd.to_timedelta(df["minute"], unit="min")
    cols = [c for c, _ in ACTIVITY]

    fig, ax = plt.subplots(figsize=(10, 4))
    ax.stackplot(t, [df[c] / 1000 for c in cols], labels=[lbl for _, lbl in ACTIVITY],
                 colors=SERIES[: len(cols)], edgecolor=SURFACE, linewidth=0.4)
    open_ns, close_ns = market_hours(csv.parent)
    for ns, name in ((open_ns, "open"), (close_ns, "close")):
        x = DAY + pd.to_timedelta(ns, unit="ns")
        ax.axvline(x, color=INK_2, linewidth=0.8, linestyle="--")
        ax.annotate(name, (x, 1), xycoords=("data", "axes fraction"),
                    xytext=(3, -2), textcoords="offset points", va="top",
                    color=INK_2)
    ax.set_title("Order book messages per minute (all symbols)")
    ax.set_ylabel("thousand messages / min")
    ax.legend(loc="upper left", ncols=len(cols), bbox_to_anchor=(0, -0.12))
    hhmm_axis(ax)

    total = df[cols].sum(axis=1)
    peak = df.loc[total.idxmax(), "minute"]
    # One-minute spikes are a single pixel wide: name the peak instead.
    peak_t = DAY + pd.to_timedelta(peak, unit="min")
    ax.annotate(f"peak {total.max() / 1000:,.0f}k/min at {peak // 60:02d}:{peak % 60:02d}",
                (peak_t, total.max() / 1000), xytext=(-8, 0),
                textcoords="offset points", ha="right", va="center", color=INK)
    ax.set_ylim(0, total.max() / 1000 * 1.08)
    print(f"activity: {int(total.sum()):,} book messages, peak "
          f"{int(total.max()):,}/min at {peak // 60:02d}:{peak % 60:02d}")
    return save(fig, out, "activity.png")


# ---------------------------------------------------------------------------
# docs/benchmarks.md History table -> optimization history

def read_history(md: Path) -> pd.DataFrame:
    """Rows of the '## History' table: version, ns/msg, LLC misses, p99 (ns)."""
    text = md.read_text(encoding="utf-8")
    section = text.split("## History", 1)[1]
    first_number = re.compile(r"[\d,]+(?:\.\d+)?")

    def num(cell: str) -> float:
        match = first_number.search(cell)
        if not match:
            raise ValueError(f"no number in {cell!r}")
        return float(match.group().replace(",", ""))

    rows = []
    for line in section.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 5 or not re.fullmatch(r"v\d+", cells[0]):
            continue
        rows.append({"version": cells[0], "ns_per_msg": num(cells[2]),
                     "llc_miss": num(cells[3]), "p99_ns": num(cells[4])})
    if not rows:
        raise ValueError(f"{md}: no rows in the History table")
    return pd.DataFrame(rows)


def plot_bench(md: Path, out: Path) -> Path:
    h = read_history(md)
    panels = [
        ("ns_per_msg", "Decode + book, ns per message", "{:.0f}"),
        ("llc_miss", "Last-level cache misses per message", "{:.1f}"),
        ("p99_ns", "p99 book update latency, ns", "{:,.0f}"),
    ]
    # Small multiples, one measure each: never two scales on one axis.
    fig, axes = plt.subplots(1, len(panels), figsize=(11, 3.4))
    for ax, (col, title, fmt) in zip(axes, panels):
        bars = ax.bar(h["version"], h[col], color=BID, width=0.6)
        ax.bar_label(bars, labels=[fmt.format(v) for v in h[col]],
                     padding=2, color=INK)
        ax.set_title(title, fontsize=10)
        ax.set_ylim(0, h[col].max() * 1.18)
        ax.tick_params(axis="x", length=0)
    fig.suptitle("Optimization history (NQ-4G slice, CPU 2, median of 3 runs)",
                 x=0.01, ha="left", fontweight="bold")
    fig.tight_layout()
    first, last = h.iloc[0], h.iloc[-1]
    print(f"bench: {first['version']} {first['ns_per_msg']:.1f} ns -> "
          f"{last['version']} {last['ns_per_msg']:.1f} ns "
          f"({1 - last['ns_per_msg'] / first['ns_per_msg']:.0%} lower)")
    return save(fig, out, "bench_history.png")


# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("what", choices=["book", "activity", "bench", "all"])
    p.add_argument("path", type=Path,
                   help="CSV / markdown file, or the itch_export directory for 'all'")
    p.add_argument("--out", type=Path, default=Path("docs/img"))
    p.add_argument("--all-day", action="store_true",
                   help="book: include pre- and post-market (default: 09:30-16:00)")
    a = p.parse_args(argv)

    try:
        if a.what == "book":
            plot_book(a.path, a.out, a.all_day)
        elif a.what == "activity":
            plot_activity(a.path, a.out)
        elif a.what == "bench":
            plot_bench(a.path, a.out)
        else:
            for csv in sorted(a.path.glob("book_*.csv")):
                try:
                    plot_book(csv, a.out, a.all_day)
                except ValueError as e:
                    print(f"  skipped: {e}")
            plot_activity(a.path / "activity.csv", a.out)
            plot_bench(Path("docs/benchmarks.md"), a.out)
    except (OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
