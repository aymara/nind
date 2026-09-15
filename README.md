# NIND — "nouvelle indexation"

[![PyPI version](https://img.shields.io/pypi/v/nind.svg)](https://pypi.org/project/nind/)
[![License: LGPL v3](https://img.shields.io/badge/license-LGPL--3.0-blue.svg)](LICENCE.md)
[![Build](https://github.com/aymara/nind/actions/workflows/build.yml/badge.svg)](https://github.com/aymara/nind/actions/workflows/build.yml)
[![Docs](https://github.com/aymara/nind/actions/workflows/docs.yml/badge.svg)](https://github.com/aymara/nind/actions/workflows/docs.yml)

**nind** is a flat-file inverted-index module: a simple, dependency-light
indexing and search engine built around plain binary files instead of a
database or a heavyweight library like Lucene. It ships as:

- a production **C++** library (`src/cpp/`), and
- a **Python package**, `nind` (`src/py/nind/`) — an ergonomic frontend to
  the same binary formats (backed by a compiled `nind._native` extension,
  pybind11 bindings over the C++ stack), plus a modern BM25-style search
  API layered on top.

📖 **Full documentation:** https://aymara.github.io/nind/

## Why nind?

In 2014, **Atejcon** designed and built nind to meet the indexing needs of
an application such as **ANT'inno**'s `ANT'box`, aiming for something
simpler and faster than the alternatives available at the time.

**CEA LIST/DIASI/LVIC** later adapted nind for its `Amose` search engine,
replacing the deprecated `S2` and `Lucene` indexers. nind was extended to
match `Amose`'s richer functional needs — in particular its more elaborate
term-type model and the various corpus-level measures its relevance
calculations require.

nind indexes and searches using nothing but flat files: no database, no
server process. Because those files are also usable directly, the indexed
corpus can be inspected and analyzed offline, with tools as simple as the
command line — see [Command-line tools](#command-line-tools) below.

To keep the binary file formats unambiguous and toolable, nind specifies
them in an **EBNF** (Extended Backus-Naur Form) grammar; class comments
throughout the source quote the relevant grammar rules directly above each
class. That the format is written down formally is also what makes an
ergonomic, non-C++ way of reading and writing the same files possible in
the first place — which is exactly what the Python package above is.

## Installation

```bash
pip install nind
```

This installs the `nind` Python package together with its compiled
`nind._native` extension (prebuilt wheels are published for Linux, macOS
and Windows). Building from source (e.g. an sdist install, or an
unsupported platform) requires a C++14 compiler; `pip`/`uv` pull in the
rest of the build toolchain (`scikit-build-core`, `pybind11`, `cmake`,
`ninja`) automatically — no manual setup needed.

Requires Python ≥ 3.8.

## Quickstart

Index a handful of files and run a BM25-ranked search over them:

```python
from nind.nind_engine import NindIndexer, NindEngine

indexer = NindIndexer(index_dir="indices", prefix="corpus")
indexer.index_files([
    "src/py/nind/NindFile.py",
    "src/py/nind/NindPadFile.py",
    "src/py/nind/nind_engine.py",
])

engine = NindEngine("indices")
for doc_id, score in engine.search("ejcrit nombre", top_k=5):
    print(f"{doc_id}: {score:.3f}")
```

`index_dir` must already exist; each input file becomes one document,
identified externally by its position in the list (`0`, `1`, `2`, ...).
`NindEngine` also exposes the building blocks it uses internally
(`get_term_id`, `get_df`, `get_tf`, `get_doc_len`) for inspecting relevance
or a specific document directly.

See the [Quickstart guide](https://aymara.github.io/nind/quickstart.html)
for more, including how to open index files produced by the C++
implementation directly.

## Command-line tools

Every low-level reader module doubles as a runnable diagnostic script
(`analyse`/`dump`/`debug` its own file format), and three higher-level
scripts operate on a whole indexed corpus:

```bash
cd src/py/nind
uv run python3 Nind_search.py <fichier lexiconindex> <terme cherché>
uv run python3 Nind_dumpDocument.py <fichier nind> <n° doc>
uv run python3 Nind_trouveMotsSansOccurrence.py <fichier>
```

See the [command-line tools guide](https://aymara.github.io/nind/cli.html)
for the full list and usage of each.

## How it works

nind's binary files are a small fixed header, one or more fixed-size
*indirection blocks* mapping an integer identifier to an `(offset,
length)` pair, the variable-length data those indirections point to, and a
trailer holding format-specific data plus an identification stamp. Every
concrete format (the lexicon, the inverted term index, the per-document
local index, the reverse lexicon) builds on that same envelope, adding
only the meaning of its own records — see the
[architecture guide](https://aymara.github.io/nind/architecture.html) for
the full module layering, on both the C++ and Python sides.

## Development

- **C++**: `NIND_DIST=/path/to/dist ./gbuild.sh` (wraps CMake + ctest;
  see `./gbuild.sh -h` for build-mode/sanitizer/generator options).
- **Python**: `uv sync` (builds `nind._native` and installs the package
  editable, plus dev dependencies), then `uv run pytest tst/py -v`.

See `CLAUDE.md` for the full architecture reference, build details, and
release process.

## License

nind is distributed under the **GNU Lesser General Public License v3**
(LGPL-3.0) — see [LICENCE.md](LICENCE.md).
