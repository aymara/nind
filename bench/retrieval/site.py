"""Write the benchmark page of the documentation site (docs/benchmarks.md) and its charts.

From driver.py's report.csv files: --current (the latest run), --previous (earlier runs, for the
systems the latest one didn't measure again). Charts: SVG, one light and one dark version each
(pydata-sphinx-theme shows the one of the reader's theme), matplotlib.

    python bench/retrieval/site.py --current WORK/report.csv --previous bench/retrieval/results/2026-11-04/report.csv \\
        --docs docs --date 2026-11-05 --version 3.2.0
"""

import argparse
import csv
import math
import os

# the systems shown, in this order: (key in report.csv, name on the page)
SYSTEMS = [("nind", "nind"), ("nind-4t", "nind, 4 threads per query"), ("nind+local", "nind + local index"),
           ("lucene", "Lucene (Anserini)"), ("lucene+pos", "Lucene + positions"), ("tantivy", "tantivy"),
           ("bm25s", "bm25s")]
NAMES = dict(SYSTEMS)
NIND = {"nind", "nind-4t", "nind+local"}
#: systems that only differ by how they search (nind's index): their build, size and update aren't shown
SEARCH_ONLY = {"nind-4t"}
SEARCH_KEYS = {"k10_p50", "k10_p99", "k1000_p50", "nDCG@10"}

# colors (validated categorical slots 1-4 of the reference palette, light and dark)
THEMES = {
    "light": {"surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
              "grid": "#e1e0d9", "axis": "#c3c2b7", "dim": "#b4b2a9",
              "series": ["#2a78d6", "#1baf7a", "#eb6834", "#eda100"]},
    "dark": {"surface": "#1a1a19", "ink": "#f0efec", "ink2": "#c3c2b7", "muted": "#898781",
             "grid": "#2c2c2a", "axis": "#383835", "dim": "#5f5e59",
             "series": ["#3987e5", "#199e70", "#d95926", "#c98500"]},
}
# the scaling chart's series and their slots (color follows the system, in every chart)
SCALING = [("nind", 0), ("nind-4t", 1), ("lucene", 2), ("tantivy", 3)]
SIZES = [("msmarco-stop-1M", 1.0), ("msmarco-stop-2M", 2.0), ("msmarco-stop-4M", 4.0), ("msmarco-stop", 8.84)]


def read(path):
    rows = {}
    if path and os.path.exists(path):
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                rows[(row["dataset"], row["system"])] = row
    return rows


def number(row, key):
    try:
        value = row.get(key, "") if row else ""
        return float(value) if value not in ("", None) else None
    except ValueError:
        return None


def fmt(value, unit=""):
    if value is None:
        return "–"
    if value >= 100:
        text = "{:,.0f}".format(value)
    elif value >= 10:
        text = "%.1f" % value
    elif value >= 1:
        text = "%.2f" % value
    else:
        text = "%.3f" % value
    return text + unit


def style(ax, theme):
    t = THEMES[theme]
    ax.set_facecolor(t["surface"])
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(t["axis"])
        ax.spines[side].set_linewidth(1)
    ax.tick_params(colors=t["muted"], labelcolor=t["ink2"], labelsize=9, length=0)
    ax.grid(True, which="major", color=t["grid"], linewidth=1, linestyle="-")
    ax.set_axisbelow(True)


def scaling_chart(rows, path, theme):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator
    t = THEMES[theme]
    plt.rcParams["font.family"] = "sans-serif"
    plt.rcParams["svg.fonttype"] = "none"
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    fig.patch.set_facecolor(t["surface"])
    style(ax, theme)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ends = []
    for system, slot in SCALING:
        points = [(size, number(rows.get((dataset, system)), "k10_p50")) for dataset, size in SIZES]
        points = [(x, y) for x, y in points if y is not None]
        if not points:
            continue
        color = t["series"][slot]
        ax.plot([x for x, _ in points], [y for _, y in points], color=color, linewidth=2,
                solid_capstyle="round", solid_joinstyle="round", label=NAMES[system], zorder=3)
        ax.plot([x for x, _ in points], [y for _, y in points], linestyle="none", marker="o", markersize=8,
                markerfacecolor=color, markeredgecolor=t["surface"], markeredgewidth=2, zorder=4)
        ends.append((points[-1][1], NAMES[system], points[-1][0]))
    # direct labels at the line ends (secondary ink), moved apart when they would overlap
    ends.sort()
    placed = []
    for y, name, x in ends:
        label_y = y
        if placed and math.log10(label_y) - math.log10(placed[-1]) < 0.07:
            label_y = placed[-1] * 10 ** 0.07
        placed.append(label_y)
        ax.annotate("%s  %s ms" % (name, fmt(y)), xy=(x, y), xytext=(x * 1.12, label_y), color=t["ink2"],
                    fontsize=9, va="center", ha="left",
                    arrowprops=dict(arrowstyle="-", color=t["axis"], linewidth=1) if label_y != y else None)
    ax.xaxis.set_major_locator(FixedLocator([s for _, s in SIZES]))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: "%gM" % v))
    ys = [y for line in ax.get_lines() for y in line.get_ydata()]
    ticks = [v for v in (0.2, 0.5, 1, 2, 5, 10, 20, 50, 100) if min(ys) / 1.5 <= v <= max(ys) * 1.5]
    ax.yaxis.set_major_locator(FixedLocator(ticks))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: "%g" % v))
    ax.set_xlim(0.85, 8.84 * 3.4)
    ax.set_xlabel("documents (MS MARCO passages, stopwords removed)", color=t["ink2"], fontsize=9)
    ax.set_ylabel("median ms per query, top 10", color=t["ink2"], fontsize=9)
    legend = ax.legend(loc="upper left", frameon=False, fontsize=9, labelcolor=t["ink2"])
    fig.tight_layout()
    fig.savefig(path, format="svg", facecolor=t["surface"])
    if os.environ.get("SITE_PNG"):         #(a PNG beside, to look at the chart)
        fig.savefig(path[:-4] + ".png", format="png", dpi=110, facecolor=t["surface"])
    plt.close(fig)


def msmarco_chart(rows, dataset, path, theme):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter
    t = THEMES[theme]
    plt.rcParams["font.family"] = "sans-serif"
    plt.rcParams["svg.fonttype"] = "none"
    panels = [("k10_p50", "search, top 10 (median ms)", " ms"), ("build_s", "build (s, 8 threads)", " s"),
              ("index_mb", "index size (MB)", " MB"), ("update_s", "update: +1% documents (s)", " s")]
    systems = [s for s, _ in SYSTEMS if any(number(rows.get((dataset, s)), key) is not None for key, _, _ in panels)]
    fig, axes = plt.subplots(1, len(panels), figsize=(10.5, 0.42 * len(systems) + 1.3), sharey=True)
    fig.patch.set_facecolor(t["surface"])
    slots = dict((s, slot) for s, slot in SCALING)
    for ax, (key, title, unit) in zip(axes, panels):
        style(ax, theme)
        ax.grid(True, axis="x")
        ax.grid(False, axis="y")
        ax.set_xscale("log")
        values = []
        for i, system in enumerate(systems):
            value = number(rows.get((dataset, system)), key)
            if value is None or value <= 0 or (system in SEARCH_ONLY and key not in SEARCH_KEYS):
                continue
            values.append(value)
            color = t["series"][slots[system]] if system in slots else (t["series"][0] if system in NIND else t["dim"])
            ax.plot([value], [i], linestyle="none", marker="o", markersize=8, markerfacecolor=color,
                    markeredgecolor=t["surface"], markeredgewidth=2, zorder=3)
            ax.annotate(fmt(value, unit), xy=(value, i), xytext=(6, 0), textcoords="offset points",
                        va="center", ha="left", fontsize=8.5, color=t["ink2"])
        if values:
            ax.set_xlim(min(values) / 2.2, max(values) * 9)
        ax.xaxis.set_major_locator(LogLocator(base=10))
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: "{:,g}".format(v)))
        ax.set_title(title, color=t["ink"], fontsize=9.5, loc="left")
        ax.set_ylim(len(systems) - 0.5, -0.5)
    axes[0].set_yticks(range(len(systems)))
    axes[0].set_yticklabels([NAMES[s] for s in systems])
    fig.tight_layout()
    fig.savefig(path, format="svg", facecolor=t["surface"])
    if os.environ.get("SITE_PNG"):         #(a PNG beside, to look at the chart)
        fig.savefig(path[:-4] + ".png", format="png", dpi=110, facecolor=t["surface"])
    plt.close(fig)


def table(rows, dataset, columns, systems=None):
    lines = ["| system | " + " | ".join(h for h, _, _ in columns) + " |", "|---|" + "---:|" * len(columns)]
    for system, name in SYSTEMS:
        if systems is not None and system not in systems:
            continue
        row = rows.get((dataset, system))
        if row is None:
            continue
        cells = []
        for _, key, unit in columns:
            value = number(row, key) if (system not in SEARCH_ONLY or key in SEARCH_KEYS) else None
            cells.append(("%.3f" % value) if (value is not None and key == "nDCG@10") else fmt(value, unit))
        lines.append("| %s | %s |" % (name, " | ".join(cells)))
    return "\n".join(lines)


def image(name, alt):
    return "\n".join(["```{image} _static/bench/%s-light.svg" % name, ":class: only-light", ":alt: " + alt, "```", "",
                      "```{image} _static/bench/%s-dark.svg" % name, ":class: only-dark", ":alt: " + alt, "```"])


PAGE = """# Benchmarks

How nind compares with other indexers when it is used as an index plus BM25 top-k search:
**Lucene** (through Anserini), **tantivy** and **bm25s**, on the same tokens, the same BM25
parameters (k1 = 1.2, b = 0.75) and the same machine. nind {version} measured on {date}, the other
systems on {previous_date} (the same data and machine; they don't depend on nind's version).

```{{admonition}} In short
:class: tip

- **Search.** On MS MARCO (8.8 million passages), one query's top 10 takes **{nind_k10} ms** on one
  thread (median), against **{lucene_k10} ms** for Lucene and **{tantivy_k10} ms** for tantivy. With
  4 threads per query (`NindEngine(..., search_threads=4)`), **{nind4_k10} ms**. The slowest 1% of
  queries take up to {nind_p99} ms ({nind4_p99} ms with 4 threads; Lucene {lucene_p99} ms).
- **Indexing.** A full build takes **{nind_build} s** (Lucene {lucene_build} s, tantivy
  {tantivy_build} s, 8 threads each); the index is **{nind_mb} MB** (Lucene {lucene_mb} MB).
- **Updates.** Adding 1% more documents takes **{nind_update} s** (Lucene {lucene_update} s,
  tantivy {tantivy_update} s, which write a new segment). This is a first update after a build,
  which rewrites the terms it touches; later updates append to them in place (see below).
- **Quality.** The same BM25: nDCG@10 {nind_ndcg} for nind, {lucene_ndcg} for Lucene (differences
  come from score ties and Lucene's 1-byte document lengths).
```

## Search latency as the corpus grows

{scaling_image}

Median time per query to get the 10 best documents, one query at a time (500 MS MARCO dev
queries per size). nind uses block-max MaxScore: only the blocks of postings that may reach the
top 10 are read. With 4 threads per query, the documents are split into 4 ranges searched at
once, with the same results.

{scaling_table}

## MS MARCO, 8.8 million passages

{msmarco_image}

{msmarco_table}

Stopwords kept (Lucene's 33 English stopwords are otherwise removed): frequent terms have very long
postings lists.

{msmarco_full_table}

## Smaller corpora (BEIR)

Every query of each dataset; stopwords removed.

{beir_table}

## How to read these numbers

- **Latency is measured per query, one at a time.** A server answering many queries at once
  gets its throughput from running them in parallel: nind's search releases Python's GIL and is
  thread-safe, so `search` can be called from many threads on one `NindEngine`. Splitting one query
  across threads (`search_threads`, `threads=`) lowers its latency on idle cores, but the cores do
  more work in total (4 threads: about 1.5–2× faster, not 4×). Keep the default (1) for
  throughput.
- **"nind, 4 threads per query" has no counterpart for the others here.** Lucene can also split a
  query (an `IndexSearcher` with an executor, over its segments); Anserini's searcher, used
  here, doesn't.
- **Lucene is called from Python** (pyjnius): its top-1000 latency mostly measures building one Java
  object per hit, so only the top 10 compares fairly.
- **nind + local index** also stores each document's terms and positions (as Amose reads them):
  larger and slower to build, the same search.
- **bm25s** precomputes every score: fast on small corpora, not built for millions of documents.
- **Updates.** nind updates its index files in place, term by term; Lucene and tantivy write a new
  segment, merged later. A build leaves no room in nind's postings lists, so the first update
  after a build rewrites every term it touches (the time above); a term rewritten by an update gets
  room, and later updates that only add documents rewrite only its last block (MS MARCO 4M: 20,000
  documents in about 5 s instead of 23 s).
- Single machine: a 14-core laptop-class processor (8 fast and 6 efficient cores), 62 GB of memory.
  Builds use 8 threads. Absolute times vary by a few percent between runs; the ratios hold.

## Reproduce

The scripts are in `bench/retrieval/` (see `bench/README.md`, section 4): `prepare.py` writes
the tokenized corpora, `driver.py` builds, searches and updates every system and writes
`report.csv`, and `site.py` writes this page from it.
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--current", required=True)
    parser.add_argument("--previous", default=None)
    parser.add_argument("--docs", default="docs")
    parser.add_argument("--date", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--previous-date", default="an earlier date",
                        help="when the systems taken from --previous were measured")
    args = parser.parse_args()
    rows = read(args.previous)
    for key, row in read(args.current).items():
        merged = dict(rows.get(key, {}))
        merged.update({field: value for field, value in row.items() if value not in ("", None)})
        rows[key] = merged
    static = os.path.join(args.docs, "_static", "bench")
    os.makedirs(static, exist_ok=True)
    for theme in THEMES:
        scaling_chart(rows, os.path.join(static, "scaling-%s.svg" % theme), theme)
        msmarco_chart(rows, "msmarco-stop", os.path.join(static, "msmarco-%s.svg" % theme), theme)

    def get(dataset, system, key, digits=None):
        value = number(rows.get((dataset, system)), key)
        if value is None:
            return "–"
        return ("%.*f" % (digits, value)) if digits is not None else fmt(value)

    scaling_rows = ["| system | " + " | ".join("%gM" % s for _, s in SIZES) + " |", "|---|" + "---:|" * len(SIZES)]
    for system, _ in SCALING:
        scaling_rows.append("| %s | %s |" % (NAMES[system], " | ".join(get(d, system, "k10_p50") + " ms" for d, _ in SIZES)))
    columns = [("top 10, median", "k10_p50", " ms"), ("top 10, 99th pct", "k10_p99", " ms"),
               ("top 1000, median", "k1000_p50", " ms"), ("build", "build_s", " s"), ("build memory", "build_mem", " MB"),
               ("index", "index_mb", " MB"), ("update +1%", "update_s", " s"), ("nDCG@10", "nDCG@10", "")]
    beir = ["| dataset | documents | " + " | ".join(NAMES[s] for s in ("nind", "lucene", "tantivy", "bm25s"))
            + " |", "|---|---:|" + "---:|" * 4]
    for dataset, label in (("scifact-stop", "SciFact"), ("nfcorpus-stop", "NFCorpus"), ("fiqa-stop", "FiQA")):
        docs = number(rows.get((dataset, "nind")), "documents")
        cells = []
        for system in ("nind", "lucene", "tantivy", "bm25s"):
            cells.append("%s ms · %s" % (get(dataset, system, "k10_p50"), get(dataset, system, "nDCG@10", 3)))
        beir.append("| %s | %s | %s |" % (label, "{:,.0f}".format(docs) if docs else "–", " | ".join(cells)))
    beir.append("")
    beir.append("Each cell: median top-10 latency · nDCG@10.")
    page = PAGE.format(
        version=args.version, date=args.date, previous_date=args.previous_date,
        nind_p99=get("msmarco-stop", "nind", "k10_p99"), nind4_p99=get("msmarco-stop", "nind-4t", "k10_p99"),
        lucene_p99=get("msmarco-stop", "lucene", "k10_p99"),
        nind_k10=get("msmarco-stop", "nind", "k10_p50"), lucene_k10=get("msmarco-stop", "lucene", "k10_p50"),
        tantivy_k10=get("msmarco-stop", "tantivy", "k10_p50"), nind4_k10=get("msmarco-stop", "nind-4t", "k10_p50"),
        nind_build=get("msmarco-stop", "nind", "build_s"), lucene_build=get("msmarco-stop", "lucene", "build_s"),
        tantivy_build=get("msmarco-stop", "tantivy", "build_s"), nind_mb=get("msmarco-stop", "nind", "index_mb"),
        lucene_mb=get("msmarco-stop", "lucene", "index_mb"), nind_update=get("msmarco-stop", "nind", "update_s"),
        lucene_update=get("msmarco-stop", "lucene", "update_s"), tantivy_update=get("msmarco-stop", "tantivy", "update_s"),
        nind_ndcg=get("msmarco-stop", "nind", "nDCG@10", 3), lucene_ndcg=get("msmarco-stop", "lucene", "nDCG@10", 3),
        scaling_image=image("scaling", "Median top-10 latency against corpus size for nind, nind with 4 threads, "
                                       "Lucene and tantivy; the same data is in the table below."),
        scaling_table="\n".join(scaling_rows),
        msmarco_image=image("msmarco", "MS MARCO 8.8M: search latency, build time, index size and update time per "
                                       "system; the same data is in the table below."),
        msmarco_table=table(rows, "msmarco-stop", columns),
        msmarco_full_table=table(rows, "msmarco", columns),
        beir_table="\n".join(beir))
    with open(os.path.join(args.docs, "benchmarks.md"), "w", encoding="utf-8") as f:
        f.write(page)
    print("wrote", os.path.join(args.docs, "benchmarks.md"))


if __name__ == "__main__":
    main()
