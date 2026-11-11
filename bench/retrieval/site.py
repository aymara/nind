"""Write the benchmark page of the documentation site (docs/benchmarks.md) and its charts.

The page leads with the server runs (MS MARCO v1 8.8M and v2 up to 138M, threads and throughput),
then keeps the laptop runs it replaced, in their own section (numbers of two machines are never mixed
in a table). From driver.py's report.csv files and the thread runs' JSON lines (see
bench/README.md, section 7). Charts: SVG, one light and one dark version each (pydata-sphinx-theme
shows the one of the reader's theme), matplotlib.

    python bench/retrieval/site.py --docs docs --date 2026-11-10 --version 3.2.0 \\
        --server bench/retrieval/results/2026-11-10-server/v1-report.csv \\
                 bench/retrieval/results/2026-11-10-server/v2-report.csv \\
        --threads bench/retrieval/results/2026-11-10-server \\
        --memory bench/retrieval/results/2026-11-10-server/memory-budget.jsonl --code-of 4.0.1 \\
        --laptop bench/retrieval/results/2026-11-04/report.csv bench/retrieval/results/2026-11-05/report.csv \\
        --laptop-date 2026-11-05 --laptop-previous-date 2026-11-04
"""

import argparse
import csv
import json
import math
import os

# the systems shown, in this order: (key in report.csv, name on the page)
SYSTEMS = [("nind", "nind"), ("nind-4t", "nind, 4 threads per query"), ("nind-8t", "nind, 8 threads per query"),
           ("nind-8s", "nind, 8 segments"), ("nind-8s-8t", "nind, 8 segments, 8 threads per query"),
           ("nind-32s", "nind, 32 segments"), ("nind-32s-8t", "nind, 32 segments, 8 threads per query"),
           ("nind+local", "nind + local index"), ("lucene", "Lucene (Anserini)"),
           ("lucene+pos", "Lucene + positions"), ("tantivy", "tantivy"), ("bm25s", "bm25s")]
NAMES = dict(SYSTEMS)
NIND = {s for s, _ in SYSTEMS if s.startswith("nind")}
#: systems that only differ by how they search an index of another one: their build, size and update aren't shown
SEARCH_ONLY = {"nind-4t", "nind-8t", "nind-8s-8t", "nind-32s-8t"}
SEARCH_KEYS = {"k10_p50", "k10_p99", "k100_p50", "k1000_p50", "nDCG@10"}

# colors (validated categorical slots 1-4 of the reference palette, light and dark)
THEMES = {
    "light": {"surface": "#fcfcfb", "ink": "#0b0b0b", "ink2": "#52514e", "muted": "#898781",
              "grid": "#e1e0d9", "axis": "#c3c2b7", "dim": "#b4b2a9",
              "series": ["#2a78d6", "#1baf7a", "#eb6834", "#eda100"]},
    "dark": {"surface": "#1a1a19", "ink": "#f0efec", "ink2": "#c3c2b7", "muted": "#898781",
             "grid": "#2c2c2a", "axis": "#383835", "dim": "#5f5e59",
             "series": ["#3987e5", "#199e70", "#d95926", "#c98500"]},
}
# the scaling charts' series and their slots (color follows the system, in every chart of a section)
SCALING = [("nind", 0), ("nind-8s-8t", 1), ("lucene", 2), ("tantivy", 3)]
SIZES = [("v2-stop-1M", 1.0), ("v2-stop-4M", 4.0), ("v2-stop-8M", 8.0), ("v2-stop-16M", 16.0),
         ("v2-stop-32M", 32.0), ("v2-stop-64M", 64.0), ("v2-stop", 138.36)]
LAPTOP_SCALING = [("nind", 0), ("nind-4t", 1), ("lucene", 2), ("tantivy", 3)]
LAPTOP_SIZES = [("msmarco-stop-1M", 1.0), ("msmarco-stop-2M", 2.0), ("msmarco-stop-4M", 4.0), ("msmarco-stop", 8.84)]


def read(paths):
    """report.csv files, later ones overriding the fields they fill: {(dataset, system): row}"""
    rows = {}
    for path in paths or []:
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                key = (row["dataset"], row["system"])
                merged = dict(rows.get(key, {}))
                merged.update({field: value for field, value in row.items() if value not in ("", None)})
                rows[key] = merged
    return rows


def jsonl(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


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


def save(fig, path, theme):
    import matplotlib
    # the same data gives the same file: no date, ids from a fixed salt
    matplotlib.rcParams["svg.hashsalt"] = os.path.basename(path)
    fig.savefig(path, format="svg", facecolor=THEMES[theme]["surface"], metadata={"Date": None})
    if os.environ.get("SITE_PNG"):         #(a PNG beside, to look at the chart)
        fig.savefig(path[:-4] + ".png", format="png", dpi=110, facecolor=THEMES[theme]["surface"])


def scaling_chart(rows, series, sizes, xlabel, path, theme):
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
    for system, slot in series:
        points = [(size, number(rows.get((dataset, system)), "k10_p50")) for dataset, size in sizes]
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
        if placed and math.log10(label_y) - math.log10(placed[-1]) < 0.1:
            label_y = placed[-1] * 10 ** 0.1
        placed.append(label_y)
        ax.annotate("%s  %s ms" % (name, fmt(y)), xy=(x, y), xytext=(x * 1.12, label_y), color=t["ink2"],
                    fontsize=9, va="center", ha="left",
                    arrowprops=dict(arrowstyle="-", color=t["axis"], linewidth=1) if label_y != y else None)
    ax.xaxis.set_major_locator(FixedLocator([s for _, s in sizes]))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: "%gM" % round(v, 1 if v < 10 else 0)))
    ys = [y for line in ax.get_lines() for y in line.get_ydata()]
    ticks = [v for v in (0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200) if min(ys) / 1.5 <= v <= max(ys) * 1.5]
    ax.yaxis.set_major_locator(FixedLocator(ticks))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: "%g" % v))
    ax.set_xlim(sizes[0][1] * 0.85, sizes[-1][1] * 3.4 ** (math.log10(sizes[-1][1] / sizes[0][1]) / math.log10(8.84)))
    ax.set_xlabel(xlabel, color=t["ink2"], fontsize=9)
    ax.set_ylabel("median ms per query, top 10", color=t["ink2"], fontsize=9)
    ax.legend(loc="upper left", frameon=False, fontsize=9, labelcolor=t["ink2"])
    fig.tight_layout()
    save(fig, path, theme)
    plt.close(fig)


def systems_chart(rows, dataset, series, path, theme):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter, LogLocator, NullFormatter
    t = THEMES[theme]
    plt.rcParams["font.family"] = "sans-serif"
    plt.rcParams["svg.fonttype"] = "none"
    panels = [("k10_p50", "search, top 10\n(median ms)", " ms"), ("build_s", "build\n(s, 8 threads)", " s"),
              ("index_mb", "index size\n(MB)", " MB"), ("update_s", "update: +1%\ndocuments (s)", " s")]
    systems = [s for s, _ in SYSTEMS if any(number(rows.get((dataset, s)), key) is not None for key, _, _ in panels)]
    fig, axes = plt.subplots(1, len(panels), figsize=(10.5, 0.42 * len(systems) + 1.3), sharey=True)
    fig.patch.set_facecolor(t["surface"])
    slots = dict(series)
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
    save(fig, path, theme)
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


def grid(header, rows, columns, cell, best=None):
    """a table of rows x columns; best(values) picks the value to set in bold in each row"""
    lines = ["| %s | %s |" % (header, " | ".join(str(c) for c in columns)), "|---|" + "---:|" * len(columns)]
    for label, values in rows:
        chosen = best([v for v in values.values() if v is not None]) if best and values else None
        cells = []
        for c in columns:
            v = values.get(c)
            text = cell(v) if v is not None else "–"
            cells.append("**%s**" % text if v is not None and v == chosen else text)
        lines.append("| %s | %s |" % (label, " | ".join(cells)))
    return "\n".join(lines)


def image(name, alt):
    return "\n".join(["```{image} _static/bench/%s-light.svg" % name, ":class: only-light", ":alt: " + alt, "```", "",
                      "```{image} _static/bench/%s-dark.svg" % name, ":class: only-dark", ":alt: " + alt, "```"])


# the thread runs' indexes: (path in WORK, label)
INDEXES = [("v1/msmarco-stop/nind", "8.8M, one index"), ("v1/msmarco-stop/nind-8s", "8.8M, 8 segments"),
           ("v1/msmarco-stop/nind-32s", "8.8M, 32 segments"),
           ("v1/msmarco/nind", "8.8M stopwords kept, one index"), ("v1/msmarco/nind-8s", "8.8M stopwords kept, 8 segments"),
           ("v1/msmarco/nind-32s", "8.8M stopwords kept, 32 segments"),
           ("v2/v2-stop/nind", "138M, one index"), ("v2/v2-stop/nind-8s", "138M, 8 segments"),
           ("v2/v2-stop/nind-32s", "138M, 32 segments"),
           ("v2/v2/nind", "138M stopwords kept, one index"), ("v2/v2/nind-8s", "138M stopwords kept, 8 segments")]


def index_key(path):
    return path.replace("work/", "", 1).replace("/index", "")


def thread_tables(directory):
    labels = dict(INDEXES)
    per_query = {}
    for r in jsonl(os.path.join(directory, "search-threads.jsonl")):
        per_query.setdefault(index_key(r["index"]), {})[r["threads"]] = r["p50_ms"]
    builds = {}
    for r in jsonl(os.path.join(directory, "build-threads.jsonl")):
        if r.get("phase") == "build" and not r.get("error"):
            builds.setdefault((r["run"].startswith("s3-build-138M"), r["system"]), {})[r["threads"]] = r
    order = [p for p, _ in INDEXES]
    pq_threads = sorted({t for v in per_query.values() for t in v})
    tables = {"per_query": grid("index", [(labels[k], per_query[k]) for k in order if k in per_query], pq_threads,
                                lambda v: fmt(v), best=min)}
    b_threads = sorted({t for (large, _), v in builds.items() if not large for t in v})
    b_rows = [(NAMES[s], {t: r["build_s"] for t, r in builds[(False, s)].items()})
              for s, _ in SYSTEMS if (False, s) in builds]
    tables["builds"] = grid("system", b_rows, b_threads, lambda v: fmt(v, " s"), best=min)
    return tables, per_query, builds


#: the systems of the several-callers runs, and their datasets: (key, label)
CALLER_SYSTEMS = ["nind", "nind-8s", "lucene", "tantivy"]
CALLER_DATASETS = [("msmarco-stop", "MS MARCO 8.8M"), ("v2-stop", "MS MARCO v2 138M")]


def callers(paths):
    """The several-callers runs (concurrency.py, every system): {(dataset, mode): {system: {callers: row}}};
    a later file's runs replace an earlier one's"""
    data = {}
    for r in (row for path in (paths or []) for row in jsonl(path)):
        dataset = "v2-stop" if "/v2/" in r["index"] else "msmarco-stop"
        data.setdefault((dataset, r["mode"]), {}).setdefault(r["system"], {})[r["callers"]] = r
    return data


def callers_table(data, dataset, mode, key="qps", cell=lambda v: "{:,.0f}".format(v), best=max):
    """callers x systems; the best system of each row in bold"""
    runs = data.get((dataset, mode), {})
    counts = sorted({c for v in runs.values() for c in v})
    systems = [s for s in CALLER_SYSTEMS if s in runs]
    lines = ["| callers | %s |" % " | ".join(NAMES[s] for s in systems), "|---:|" + "---:|" * len(systems)]
    for c in counts:
        values = [runs[s].get(c, {}).get(key) for s in systems]
        chosen = best([v for v in values if v is not None]) if best else None
        lines.append("| %d | %s |" % (c, " | ".join("–" if v is None else ("**%s**" % cell(v) if v == chosen else cell(v))
                                                     for v in values)))
    return "\n".join(lines)


CALLERS_TEXT = """**Many callers at once**, every system: N callers, each sending its next query when the last
one returns (`bench/retrieval/concurrency.py`), top 10, stopwords removed; queries per second, the best
of each row in bold. Threads share one searcher (nind, Lucene through pyjnius and tantivy-py release
Python's GIL while they search); processes each open the index, as separate servers would. Pinned as
above. {code}

MS MARCO 8.8M, threads:

{v1_threads}

MS MARCO v2 138M, threads:

{v2_threads}

Processes, 8.8M:

{v1_processes}

Processes, 138M:

{v2_processes}

99th percentile at 138M, threads, ms:

{v2_p99}

- **8.8M: nind serves the most at every load**, threads or processes: {v1_nind_12} queries/s with 12
  callers (one socket's cores; Lucene {v1_lucene_12}), {v1_nind_48} with 48 threads (Lucene's lose past the
  cores: {v1_lucene_48}), {v1_nind_p48} with 48 processes (Lucene {v1_lucene_p48}, each with its own JVM).
- **138M: nind leads with few callers, Lucene with many**: {v2_nind_1} against {v2_lucene_1} queries/s
  alone, {v2_nind_48} (8 segments: {v2_8s_48}) against {v2_lucene_48} with 48 threads. nind gains less from
  more threads on the large index ({v2_nind_24} to {v2_nind_48} from 24 to 48 callers, Lucene
  {v2_lucene_24} to {v2_lucene_48}); `docs/TODO.md`. 99th percentile alone: {v2_nind_p99} ms, Lucene's
  {v2_lucene_p99} ms. (Before MaxScore scanned queries of 5 terms or more in windows, term at a time,
  nind served {v2_nind_48_before} with 48 threads at 138M and {v1_nind_48_before} at 8.8M.)
- **Segments** serve about as many queries as one index up to 24 callers (each query does the same
  work; they lower latency with threads per query, above), more with 48 threads at 138M
  ({v2_8s_48} against {v2_nind_48}).
- **Processes** each run their share of the queries after their own warm-up: with many of them, few
  queries each (Lucene's JVMs warm up apart), so those rows vary more.
- **Memory of its own per process** (anonymous memory, 138M): {anonymous}. A system that maps its
  index (Lucene, tantivy) shares those pages between processes, as nind shares its mapped document
  names; nind's are mostly its documents' lengths and its directory cache."""


def callers_section(paths, anonymous_path, code):
    data = callers(paths)
    before = callers(paths[:1]) if paths and len(paths) > 1 else data
    if not data:
        return "", {"c_v1_nind_12": "–", "c_v1_lucene_12": "–", "c_v1_nind_48": "–", "c_v1_lucene_48": "–",
                    "c_v2_lucene_48": "–", "c_v2_nind_48": "–"}

    def value(dataset, mode, system, callers_nb, key="qps"):
        r = data.get((dataset, mode), {}).get(system, {}).get(callers_nb)
        return r[key] if r else None

    def qps(dataset, mode, system, callers_nb):
        v = value(dataset, mode, system, callers_nb)
        return "{:,.0f}".format(v) if v is not None else "–"

    anonymous = []
    for r in (jsonl(anonymous_path) if anonymous_path else []):
        if r.get("callers") == 1 and "anonymous_gb" in r:
            anonymous.append("%s %.2f GB" % (NAMES[r["system"]], r["anonymous_gb"]))
    v2_nind_48, v2_lucene_48 = value("v2-stop", "threads", "nind", 48), value("v2-stop", "threads", "lucene", 48)
    text = CALLERS_TEXT.format(
        code=code,
        v1_threads=callers_table(data, "msmarco-stop", "threads"), v2_threads=callers_table(data, "v2-stop", "threads"),
        v1_processes=callers_table(data, "msmarco-stop", "processes"),
        v2_processes=callers_table(data, "v2-stop", "processes"),
        v2_p99=callers_table(data, "v2-stop", "threads", key="p99_ms", cell=lambda v: "{:,.0f}".format(v), best=min),
        v1_nind_12=qps("msmarco-stop", "threads", "nind", 12), v1_lucene_12=qps("msmarco-stop", "threads", "lucene", 12),
        v1_nind_48=qps("msmarco-stop", "threads", "nind", 48), v1_lucene_48=qps("msmarco-stop", "threads", "lucene", 48),
        v1_nind_p48=qps("msmarco-stop", "processes", "nind", 48),
        v1_lucene_p48=qps("msmarco-stop", "processes", "lucene", 48),
        v2_nind_1=qps("v2-stop", "threads", "nind", 1), v2_lucene_1=qps("v2-stop", "threads", "lucene", 1),
        v2_8s_48=qps("v2-stop", "threads", "nind-8s", 48),
        v2_nind_p99=fmt(value("v2-stop", "threads", "nind", 1, "p99_ms")),
        v2_lucene_p99=fmt(value("v2-stop", "threads", "lucene", 1, "p99_ms")),
        v2_nind_24=qps("v2-stop", "threads", "nind", 24), v2_nind_48=qps("v2-stop", "threads", "nind", 48),
        v2_lucene_24=qps("v2-stop", "threads", "lucene", 24), v2_lucene_48=qps("v2-stop", "threads", "lucene", 48),
        v2_nind_48_before="{:,.0f}".format(before[("v2-stop", "threads")]["nind"][48]["qps"]),
        v1_nind_48_before="{:,.0f}".format(before[("msmarco-stop", "threads")]["nind"][48]["qps"]),
        anonymous="; ".join(anonymous) if anonymous else "not measured")
    return text, {"c_v1_nind_12": qps("msmarco-stop", "threads", "nind", 12),
                  "c_v1_lucene_12": qps("msmarco-stop", "threads", "lucene", 12),
                  "c_v1_nind_48": qps("msmarco-stop", "threads", "nind", 48),
                  "c_v1_lucene_48": qps("msmarco-stop", "threads", "lucene", 48),
                  "c_v2_lucene_48": qps("v2-stop", "threads", "lucene", 48),
                  "c_v2_nind_48": qps("v2-stop", "threads", "nind", 48)}


#: the systems searched under a page-cache budget
MEMORY_SYSTEMS = ["nind", "nind-8s-8t", "lucene", "tantivy"]
MEMORY_TEXT = """MS MARCO v2, 138M passages, stopwords removed (indexes of 11 to 15 GB): each system searches in a
cgroup (`systemd-run --user -p MemoryMax=...`) holding its own memory (the peak of an uncapped run,
+10%) plus a budget for the page cache; the index is evicted from the cache first, so every page
it reads counts. The first 2,000 queries after a warm-up of 500; median ms.

{top10}

Top 1000, median ms:

{top1000}

- **One thread on nind's index barely reads.** MaxScore reads only the blocks it needs, with
  positioned reads: about 1.3 MB per query from disk with a 1 GB budget (1.0 MB from a cold cache
  without one), which the SSDs serve without changing the median. (Its uncapped run is the slower
  one: separate processes vary, see "How to read these numbers".)
- **8 segments with 8 threads per query** read more per query (2.3 MB at 1 GB) in a few
  milliseconds: the disk shows, from {s8_all} ms without a limit to {s8_8} ms with 8 GB and {s8_1} ms with
  {low} GB, about the single thread's time.
- **Lucene and tantivy map their files** (`mmap`): with less cache, Lucene's top 10 gets
  {lucene_ratio}× slower and its top 1000 (which loads each hit's stored id) much more; tantivy's top
  1000 too. Readahead on mapped files is a likely cause, not investigated.
- **nind's own memory is the limit**: {nind_own} GB before any cache. At open it reads the document
  names ({names_gb} GB) and lengths ({lengths_gb} GB) files whole; Lucene needs {lucene_own} GB and
  tantivy {tantivy_own} GB. Reading them on demand (`mmap`) is in `docs/TODO.md`."""


def memory(path):
    rows = jsonl(path) if path else []
    if not rows:
        return "Not measured.", {k: "–" for k in ("mem_nind_1", "mem_lucene_all", "mem_lucene_1", "mem_nind_own",
                                                  "mem_lucene_own")}
    by = {(r["system"], r.get("budget_gb")): r for r in rows}
    budgets = [None] + sorted({r["budget_gb"] for r in rows if r.get("budget_gb")}, reverse=True)
    label = {None: "no limit"}
    label.update({b: "%d GB" % b for b in budgets if b})

    def own(system):
        r = by.get((system, None))
        return r["cgroup"]["anon_peak"] / 2 ** 30 if r else None

    def grid_of(key):
        lines = ["| system | own memory | " + " | ".join(label[b] for b in budgets) + " |",
                 "|---|---:|" + "---:|" * len(budgets)]
        for system in MEMORY_SYSTEMS:
            cells = [fmt(by[(system, b)][key]["p50_ms"]) if (system, b) in by else "–" for b in budgets]
            lines.append("| %s | %s GB | %s |" % (NAMES[system], fmt(own(system)), " | ".join(cells)))
        return "\n".join(lines)

    def k10(system, budget):
        r = by.get((system, budget))
        return r["k10"]["p50_ms"] if r else None

    low = budgets[-1]
    text = MEMORY_TEXT.format(top10=grid_of("k10"), top1000=grid_of("k1000"),
                              lucene_ratio="%.0f" % (k10("lucene", low) / k10("lucene", None)),
                              nind_own=fmt(own("nind")), lucene_own=fmt(own("lucene")), tantivy_own=fmt(own("tantivy")),
                              names_gb="4.96", lengths_gb="1.66", low=low, s8_all=fmt(k10("nind-8s-8t", None)),
                              s8_8=fmt(k10("nind-8s-8t", budgets[1])), s8_1=fmt(k10("nind-8s-8t", low)))
    return text, {"mem_nind_1": fmt(k10("nind", low)), "mem_lucene_all": fmt(k10("lucene", None)),
                  "mem_lucene_1": fmt(k10("lucene", low)), "mem_nind_own": fmt(own("nind")),
                  "mem_lucene_own": fmt(own("lucene"))}


PAGE = """# Benchmarks

How nind compares with other indexers when it is used as an index plus BM25 top-k search:
**Lucene** (through Anserini), **tantivy** and **bm25s**, on the same tokens, the same BM25
parameters (k1 = 1.2, b = 0.75) and the same machine, from 3,600 documents to **138 million** passages.
nind {version}{code_of}, measured on a 48-thread server from 2026-11-05 to {date}. The laptop results this page
showed before are kept at the end.

```{{admonition}} In short
:class: tip

- **Search, 138 million passages** (MS MARCO v2). One query's top 10 takes **{v2_nind} ms** on one
  thread (median), the same as Lucene ({v2_lucene} ms; tantivy {v2_tantivy} ms). An index of 8
  segments searched with 8 threads per query takes **{v2_8s8t} ms**. 99th percentile: {v2_nind_p99} ms on
  one thread (Lucene {v2_lucene_p99} ms), {v2_8s8t_p99} ms with 8 segments and 8 threads.
- **Search, 8.8 million passages** (MS MARCO). **{v1_nind} ms** on one thread (Lucene {v1_lucene} ms,
  tantivy {v1_tantivy} ms), **{v1_8s8t} ms** with 8 segments and 8 threads.
- **Stopwords kept** is nind's weak case: very long postings lists. At 138M, {v2f_nind} ms on one thread
  against {v2f_lucene} ms for Lucene; {v2f_8s8t} ms with 8 segments and 8 threads.
- **Many callers at once**, every system: at 8.8M nind serves the most at every load ({c_v1_nind_12}
  queries/s with 12 callers, Lucene {c_v1_lucene_12}; {c_v1_nind_48} with 48, Lucene's threads
  {c_v1_lucene_48}). At 138M nind leads with few callers and Lucene with many ({c_v2_lucene_48} queries/s
  with 48 callers, nind {c_v2_nind_48}).
- **Index larger than memory.** With 1 GB of page cache for a 14 GB index, nind's top 10 stays at
  {mem_nind_1} ms (it reads about 1 MB per query); Lucene's goes from {mem_lucene_all} to {mem_lucene_1} ms.
  But nind holds {mem_nind_own} GB of its own (document names and lengths, read whole at open), Lucene
  {mem_lucene_own} GB.
- **Indexing.** 138M passages in **{v2_32s_build} s** in 32 segments (Lucene {v2_lucene_build} s,
  tantivy {v2_tantivy_build} s, 8 threads each), **{v2_32s_build48} s** with 48 threads; one index:
  {v2_nind_build} s and {v2_nind_mem} GB of memory. 8.8M passages: {v1_32s_build} s in 32 segments,
  {v1_nind_build} s for one index (Lucene {v1_lucene_build} s).
- **Updates.** Adding 1% more documents writes a new segment: {v1_nind_update} s at 8.8M (Lucene
  {v1_lucene_update} s, tantivy {v1_tantivy_update} s), {v2_nind_update} s at 138M (Lucene
  {v2_lucene_update} s, tantivy {v2_tantivy_update} s).
- **Quality.** The same BM25: nDCG@10 {v1_nind_ndcg} for nind and {v1_lucene_ndcg} for Lucene at 8.8M,
  {v2_nind_ndcg} and {v2_lucene_ndcg} at 138M; every nind layout returns the same top 10.
```

## Search latency as the corpus grows

{scaling_image}

Median time per query to get the 10 best documents, one query at a time: prefixes of MS MARCO v2
(stopwords removed), its 3,903 dev queries. nind uses block-max MaxScore: only the blocks of postings
that may reach the top 10 are read. Up to 64M passages nind is faster than Lucene on one thread; at
138M they are even. With 8 segments and 8 threads per query, each thread searches a segment, with the
same results.

{scaling_table}

## MS MARCO v2, 138 million passages

{v2_image}

{v2_table}

Stopwords kept (Lucene's 33 English stopwords are otherwise removed): frequent terms have very long
postings lists (a term in most of the 138M passages). Not updated.

{v2_full_table}

The relevance judgments of MS MARCO v2's dev queries are sparse (about one passage per query), hence the
low nDCG@10 for every system. The top 10 of Lucene and nind overlap at {v2_overlap_lucene} (tantivy
{v2_overlap_tantivy}). `verify_index` read back every definition of the one-index and 8-segment builds without error
(4.0G postings; 5.2G with stopwords).

## MS MARCO, 8.8 million passages

{v1_image}

{v1_table}

Stopwords kept:

{v1_full_table}

bm25s: the first 500 queries (it scores every document), hence its different nDCG@10.

## Threads

**Threads per query**, one query at a time (`NindEngine.search(..., threads=t)`): median ms, top 10.
Up to 12 threads on one socket's physical cores, 16 and 24 on both sockets', 48 on every hardware
thread (here and below). Each run is a process of its own (see "Separate processes vary" below:
2.10 ms here for the 2.66 ms of the table above).

{per_query_table}

A single index splits its documents into ranges, one per thread; its gain stops at 8 threads at 8.8M
(4 with stopwords kept) and at 4 at 138M. Segments scale further: each thread searches its
own segments, sharing the top 10's threshold. Crossing to the second socket costs more than it gives.

{callers_section}

**Build threads**, 8.8M passages (`--threads`): seconds.

{builds_table}

A build in 32 segments runs its shards in parallel: it scales to every hardware thread (peak memory
{mem_32s_1} GB on 1 thread, {mem_32s_48} GB on 48). One index has a single writer, so threads only
tokenize. At 138M with 48 threads: nind in 32 segments {b138_32s} s (peak {b138_32s_mem} GB), Lucene
{b138_lucene} s, tantivy {b138_tantivy} s (both slower than with 8 threads on one socket).

## Index larger than memory

{memory_section}

## Smaller corpora (BEIR)

Every query of each dataset; stopwords removed.

{beir_table}

## How to read these numbers

- **Latency is measured per query, one at a time**: the median of every dev query (3,903 for
  MS MARCO v2, 6,980 for v1), the 99th percentile for the slowest. Runs are pinned to one socket's
  12 physical cores with its memory (`numactl`); the corpus is evicted from the page cache and the
  index read once before searching; a warm-up (200 queries at 8.8M, a whole pass on v2), then 3
  measured passes at 8.8M (the median shown; they differ by a few percent), one on v2's prefixes and
  with stopwords kept, 2 at 138M.
- **Threads per query have no counterpart for the others here.** Lucene can also split a query (an
  `IndexSearcher` with an executor, over its segments); Anserini's searcher, used here, doesn't.
- **Segments** are what `index_jsonl(shards=N)` and updates leave (`docs/proposals/segments.md`): a
  build of N shards in parallel, searched as one index with the same results. The first update of a
  32-segment build merges a tier of them at 8.8M ({v1_32s_update} s); at 138M segments that large are
  left alone.
- **Lucene is called from Python** (pyjnius): its top-1000 latency mostly measures building one Java
  object per hit, so the top 10 and top 100 compare best.
- **nind + local index** also stores each document's terms and positions (as Amose reads them):
  larger and slower to build, the same search.
- **bm25s** precomputes every score: fast on small corpora, not built for millions of documents (left
  out at 138M).
- **Separate processes vary.** The same search in a new process sometimes runs about 20% faster
  (2.1 instead of 2.7 ms at 8.8M), for reasons not found yet; the passes of one process agree.
- Server: 2 × Intel Xeon Gold 5317 (12 cores, 24 hardware threads, one memory node each), 251 GB of
  memory, SATA SSDs, Linux 5.14, Java 21. Builds use 8 threads unless said otherwise.

## Earlier results: laptop

nind {laptop_version} on {laptop_date}, the other systems on {laptop_previous_date}: a 14-core
laptop-class processor (8 fast and 6 efficient cores), 62 GB of memory, MS MARCO v1 only, 500 queries
per size (hence a different nDCG@10), unpinned. The ratios mostly match the server's; absolute times are lower there (faster
cores).

{laptop_scaling_image}

{laptop_scaling_table}

{laptop_image}

{laptop_table}

Stopwords kept:

{laptop_full_table}

BEIR:

{laptop_beir_table}

nind's update numbers there are those of an index written in place (before segments): a first update
after a build rewrote every term it touched.

## Reproduce

The scripts are in `bench/retrieval/` (see `bench/README.md`, sections 4 and 7): `prepare.py` writes
the tokenized corpora, `driver.py` builds, searches and updates every system and writes
`report.csv`, `concurrency.py` measures throughput, and `site.py` writes this page from them. The
server runs' raw results and scripts are in `bench/retrieval/results/2026-11-10-server/`.
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--server", nargs="+", required=True, help="the server's report.csv files")
    parser.add_argument("--threads", required=True,
                        help="directory of search-threads.jsonl, build-threads.jsonl")
    parser.add_argument("--laptop", nargs="+", required=True, help="the laptop's report.csv files, oldest first")
    parser.add_argument("--docs", default="docs")
    parser.add_argument("--date", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--memory", default=None, help="memory-budget.jsonl: searches under a page-cache budget")
    parser.add_argument("--callers", nargs="*", default=None,
                        help="concurrency.py's runs, several callers, every system (a later file's replace an earlier one's)")
    parser.add_argument("--callers-anonymous", default=None,
                        help="concurrency.py's run with --mode processes giving each system's memory per process")
    parser.add_argument("--callers-code", default="", help="which nind the several-callers runs used (a sentence)")
    parser.add_argument("--code-of", default=None, help="a later version with the same code as --version")
    parser.add_argument("--laptop-date", required=True)
    parser.add_argument("--laptop-version", default="3.2.0")
    parser.add_argument("--laptop-previous-date", required=True,
                        help="when the laptop's other systems were measured")
    args = parser.parse_args()
    rows = read(args.server)
    laptop = read(args.laptop)
    static = os.path.join(args.docs, "_static", "bench")
    os.makedirs(static, exist_ok=True)
    for theme in THEMES:
        scaling_chart(rows, SCALING, SIZES, "documents (MS MARCO v2 passages, stopwords removed)",
                      os.path.join(static, "scaling-%s.svg" % theme), theme)
        systems_chart(rows, "v2-stop", SCALING, os.path.join(static, "msmarco-v2-%s.svg" % theme), theme)
        systems_chart(rows, "msmarco-stop", SCALING, os.path.join(static, "msmarco-%s.svg" % theme), theme)
        scaling_chart(laptop, LAPTOP_SCALING, LAPTOP_SIZES, "documents (MS MARCO passages, stopwords removed)",
                      os.path.join(static, "laptop-scaling-%s.svg" % theme), theme)
        systems_chart(laptop, "msmarco-stop", LAPTOP_SCALING, os.path.join(static, "laptop-msmarco-%s.svg" % theme),
                      theme)

    def getter(source):
        def get(dataset, system, key, digits=None, scale=1):
            value = number(source.get((dataset, system)), key)
            if value is None:
                return "–"
            return ("%.*f" % (digits, value / scale)) if digits is not None else fmt(value / scale)
        return get

    get = getter(rows)

    def scaling_table(source, series, sizes):
        g = getter(source)
        lines = ["| system | " + " | ".join("%gM" % round(s, 1 if s < 10 else 0) for _, s in sizes) + " |",
                 "|---|" + "---:|" * len(sizes)]
        for system, _ in series:
            lines.append("| %s | %s |" % (NAMES[system], " | ".join(
                (g(d, system, "k10_p50") + " ms") if g(d, system, "k10_p50") != "–" else "–" for d, _ in sizes)))
        return "\n".join(lines)

    columns = [("top 10, median", "k10_p50", " ms"), ("top 10, 99th pct", "k10_p99", " ms"),
               ("top 100, median", "k100_p50", " ms"), ("top 1000, median", "k1000_p50", " ms"),
               ("build", "build_s", " s"), ("build memory", "build_mem", " MB"), ("index", "index_mb", " MB"),
               ("update +1%", "update_s", " s"), ("nDCG@10", "nDCG@10", "")]
    laptop_columns = [c for c in columns if c[1] != "k100_p50"]

    def beir(source):
        g = getter(source)
        lines = ["| dataset | documents | " + " | ".join(NAMES[s] for s in ("nind", "lucene", "tantivy", "bm25s"))
                 + " |", "|---|---:|" + "---:|" * 4]
        for dataset, label in (("scifact-stop", "SciFact"), ("nfcorpus-stop", "NFCorpus"), ("fiqa-stop", "FiQA")):
            docs = number(source.get((dataset, "nind")), "documents")
            cells = ["%s ms · %s" % (g(dataset, s, "k10_p50"), g(dataset, s, "nDCG@10", 3))
                     for s in ("nind", "lucene", "tantivy", "bm25s")]
            lines.append("| %s | %s | %s |" % (label, "{:,.0f}".format(docs) if docs else "–", " | ".join(cells)))
        lines += ["", "Each cell: median top-10 latency · nDCG@10."]
        return "\n".join(lines)

    tables, per_query, builds = thread_tables(args.threads)
    memory_section, memory_values = memory(args.memory)
    callers_text, callers_values = callers_section(args.callers, args.callers_anonymous, args.callers_code)

    def build(large, system, threads, key="build_s", scale=1):
        r = builds.get((large, system), {}).get(threads)
        return fmt(r[key] / scale) if r and r.get(key) is not None else "–"

    def overlap(system):
        value = number(rows.get(("v2-stop", system)), "vs_nind")
        return "%.3f" % value if value is not None else "–"

    page = PAGE.format(
        version=args.version, date=args.date, code_of=" (the same code as %s)" % args.code_of if args.code_of else "",
        memory_section=memory_section, **memory_values, laptop_version=args.laptop_version, laptop_date=args.laptop_date,
        laptop_previous_date=args.laptop_previous_date,
        v2_nind=get("v2-stop", "nind", "k10_p50"), v2_lucene=get("v2-stop", "lucene", "k10_p50"),
        v2_tantivy=get("v2-stop", "tantivy", "k10_p50"), v2_8s8t=get("v2-stop", "nind-8s-8t", "k10_p50"),
        v2_nind_p99=get("v2-stop", "nind", "k10_p99"), v2_lucene_p99=get("v2-stop", "lucene", "k10_p99"),
        v2_8s8t_p99=get("v2-stop", "nind-8s-8t", "k10_p99"),
        v1_nind=get("msmarco-stop", "nind", "k10_p50"), v1_lucene=get("msmarco-stop", "lucene", "k10_p50"),
        v1_tantivy=get("msmarco-stop", "tantivy", "k10_p50"), v1_8s8t=get("msmarco-stop", "nind-8s-8t", "k10_p50"),
        v2f_nind=get("v2", "nind", "k10_p50"), v2f_lucene=get("v2", "lucene", "k10_p50"),
        v2f_8s8t=get("v2", "nind-8s-8t", "k10_p50"),
        callers_section=callers_text, **callers_values,
        v2_32s_build=get("v2-stop", "nind-32s", "build_s"), v2_lucene_build=get("v2-stop", "lucene", "build_s"),
        v2_tantivy_build=get("v2-stop", "tantivy", "build_s"), v2_32s_build48=build(True, "nind-32s", 48),
        v2_nind_build=get("v2-stop", "nind", "build_s"), v2_nind_mem=get("v2-stop", "nind", "build_mem", 0, 1024),
        v1_32s_build=get("msmarco-stop", "nind-32s", "build_s"), v1_nind_build=get("msmarco-stop", "nind", "build_s"),
        v1_lucene_build=get("msmarco-stop", "lucene", "build_s"),
        v1_nind_update=get("msmarco-stop", "nind", "update_s"), v1_lucene_update=get("msmarco-stop", "lucene", "update_s"),
        v1_tantivy_update=get("msmarco-stop", "tantivy", "update_s"),
        v2_nind_update=get("v2-stop", "nind", "update_s"), v2_lucene_update=get("v2-stop", "lucene", "update_s"),
        v2_tantivy_update=get("v2-stop", "tantivy", "update_s"),
        v1_32s_update=get("msmarco-stop", "nind-32s", "update_s"),
        v1_nind_ndcg=get("msmarco-stop", "nind", "nDCG@10", 3), v1_lucene_ndcg=get("msmarco-stop", "lucene", "nDCG@10", 3),
        v2_nind_ndcg=get("v2-stop", "nind", "nDCG@10", 4), v2_lucene_ndcg=get("v2-stop", "lucene", "nDCG@10", 4),
        v2_overlap_lucene=overlap("lucene"), v2_overlap_tantivy=overlap("tantivy"),
        scaling_image=image("scaling", "Median top-10 latency against corpus size, 1M to 138M passages, for nind, "
                                       "nind in 8 segments with 8 threads, Lucene and tantivy; the same data is in "
                                       "the table below."),
        scaling_table=scaling_table(rows, [(s, None) for s in ("nind", "nind-8t", "nind-8s-8t", "nind-32s-8t", "lucene",
                                                             "tantivy")], SIZES),
        v2_image=image("msmarco-v2", "MS MARCO v2 138M: search latency, build time, index size and update time per "
                                     "system; the same data is in the table below."),
        v2_table=table(rows, "v2-stop", columns), v2_full_table=table(rows, "v2", columns),
        v1_image=image("msmarco", "MS MARCO 8.8M: search latency, build time, index size and update time per "
                                  "system; the same data is in the table below."),
        v1_table=table(rows, "msmarco-stop", columns), v1_full_table=table(rows, "msmarco", columns),
        per_query_table=tables["per_query"], builds_table=tables["builds"],
        mem_32s_1=build(False, "nind-32s", 1, "peak_mb", 1024), mem_32s_48=build(False, "nind-32s", 48, "peak_mb", 1024),
        b138_32s=build(True, "nind-32s", 48), b138_32s_mem=build(True, "nind-32s", 48, "peak_mb", 1024),
        b138_lucene=build(True, "lucene", 48), b138_tantivy=build(True, "tantivy", 48),
        beir_table=beir(rows),
        laptop_scaling_image=image("laptop-scaling", "Laptop: median top-10 latency against corpus size for nind, "
                                                     "nind with 4 threads, Lucene and tantivy; the same data is in "
                                                     "the table below."),
        laptop_scaling_table=scaling_table(laptop, LAPTOP_SCALING, LAPTOP_SIZES),
        laptop_image=image("laptop-msmarco", "Laptop, MS MARCO 8.8M: search latency, build time, index size and "
                                             "update time per system; the same data is in the table below."),
        laptop_table=table(laptop, "msmarco-stop", laptop_columns),
        laptop_full_table=table(laptop, "msmarco", laptop_columns),
        laptop_beir_table=beir(laptop))
    with open(os.path.join(args.docs, "benchmarks.md"), "w", encoding="utf-8") as f:
        f.write(page)
    print("wrote", os.path.join(args.docs, "benchmarks.md"))


if __name__ == "__main__":
    main()
