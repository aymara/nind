# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

NIND ("nouvelle indexation") is a flat-file inverted-index module originally built for the `Amose` search engine (replacing the deprecated `S2` and `Lucene` indexers). It has two parallel implementations of the same binary formats:

- **C++** (`src/cpp/`): the production indexing/search libraries.
- **Python** (`src/py/nind/`): a pure-Python re-implementation used both as a standalone frontend and, per the original design intent, to independently verify the C++ binary output against the format's EBNF grammar (no shared code between the two — they are two independent readers/writers of the same file formats).

The repo is mid-refactor (branch `refactoring-for-codecommons`): the Python package was recently moved from `src/py/*.py` into a proper `src/py/nind/` package installable via `pyproject.toml`/hatchling, and a new higher-level `nind_engine.py` (BM25-style search API) is being layered on top of the low-level Python classes.

## Build & test — C++

Requires the `NIND_DIST` env var (install prefix). Build via the wrapper script, not raw `cmake`/`make` directly:

```
NIND_DIST=/path/to/dist ./gbuild.sh              # Debug build (default), builds + runs ctest + installs
./gbuild.sh -m Release                            # Release or RelWithDebInfo
./gbuild.sh -a ON                                 # with AddressSanitizer (+ UB/leak sanitizers where supported)
./gbuild.sh -n native                             # -march=native instead of generic tuning
./gbuild.sh -p false                              # disable parallel build (build with -j1)
./gbuild.sh -G Unix                               # use Unix Makefiles instead of Ninja
```

Build artifacts land in `build/<git-branch>/<mode>/<project>` (isolated per branch/mode, so switching branches doesn't require a clean).

To run tests manually instead of via the full `gbuild.sh` cycle: `ctest` from inside that build directory (only applies to generators that produce a `make test`/ctest target, e.g. `Unix Makefiles`; Ninja builds skip the automatic test step in `gbuild.sh`).

`-DDEBUG_NIND` is added automatically in `Debug` and `RelWithDebInfo` modes (and in `Release` if configured with `-DWITH_DEBUG_MESSAGES=ON`) to enable conditional debug output.

## Build & test — Python

The Python package (`nind`, in `src/py/nind`) is managed with `pyproject.toml`/hatchling; install/manage it with `uv` (the old `src/py/CMakeLists.txt` build path is kept only for compatibility). `uv sync` installs the package (editable) plus dev dependencies (pytest) into `.venv`.

Tests live in `tst/py/` (one file per module, mirroring `tst/cpp/unit/`), configured via `[tool.pytest.ini_options]` in `pyproject.toml`:

```
uv sync                 # first time / after touching pyproject.toml
uv run pytest tst/py -v
```

Verification of the Python readers/writers against the C++-produced binary files (the project's original EBNF-grammar cross-check design, see README) isn't automated yet — the current suite validates the Python implementation against itself (round-trips through `nind_engine.NindIndexer`'s writers, plus hand-built fixtures for formats it doesn't write, like `.nindretrolexicon`).

## Architecture

### C++ library layering (`src/cpp/`)

Each subdirectory builds one shared library; dependencies flow strictly downward (see each module's `CMakeLists.txt` `target_link_libraries`):

```
NindBasics   (no deps)      — NindFile: buffered binary sequential file I/O; NindPadFile: ident <-> data mapping on top of it
   ^
   ├── NindRetrolexicon     — flat-file reverse lexicon (id -> string)
   ├── NindLexicon          — in-memory lexicon (asymmetric maps), NindLexiconFile for its on-disk form
   │
NindIndex (deps: NindBasics, NindRetrolexicon)
   — NindLexiconIndex/NindLexiconIndexK: lexicon as an index file (K variant uses two 32-bit keys instead of strings)
   — NindTermIndex: the inverted file itself, as an index file
   — NindLocalIndex: per-document local index file
   — NindIndex: ties the above into a full inverted/local index file

NindAmose (deps: NindIndex, NindRetrolexicon)
   — adapts nind to Amose's richer term-type model (see internal doc refs "LAT2015.JYS.448")
```

All C++ code lives in namespace `latecon::nindex`. Class/file naming mirrors the binary format they implement (e.g. `NindLexiconIndex` reads/writes the `.nindlexiconindex` file format) — this naming convention holds across both the C++ and Python trees, so a class name tells you both its role and its file extension.

Design docs are referenced by internal report codes in file header comments (e.g. `LAT2014.JYS.440` = "nind, indexation post-S2", `LAT2015.JYS.448` = "Adaptation de l'indexation nind au moteur de recherche Amose") — these aren't in the repo, so treat the header comments above each class as the closest available spec.

Linker flags (`-Wl,-z,defs,--no-as-needed`) are set project-wide because these libraries are loaded dynamically via a plugin/factory mechanism, so symbols must stay visible even when not directly referenced by the linking binary — don't "clean up" apparently-unused exports without checking for this.

`tst/cpp/` mirrors the module structure for test binaries, but note `tst/cpp/CMakeLists.txt` currently only builds `NindIndex`, `NindSearch`, and `NindAmose` tests — the `NindLexicon`, `NindTests`, and `NindBasics` test subdirectories are commented out.

### Python package (`src/py/nind/`)

Parallels the C++ layering with lowercase-suffixed module names for the index classes:

- `NindFile.py` — low-level binary codec. Historically read-only-oriented (the `litXxx` getters have long existed for every format), but now has a full writer counterpart too: `ejcritNombre1/3/4/5`, `ejcritNombreULat`/`ejcritNombreSLat` (both varint encodings, full tier range matching the readers and the C++ implementation), `ejcritChaine`, `ejcritZejros`.
- `NindPadFile.py` — the shared "pad file" envelope (header, indirection block(s), specifics, identification trailer); read-only, no writer (mirrors `NindBasics::NindPadFile`, but nothing in Python subclasses it as a writer the way `NindIndex`/`NindRetrolexicon` do in C++).
- `NindRetrolexicon.py`, `NindLexiconindex.py`, `NindTermindex.py`, `NindLocalindex.py` — read-only mirrors of the corresponding `NindIndex`-family C++ classes; `NindLexiconindex` is hash-bucketed (`clefB(word) % nombreIndirection`), the other two are directly indexed by id.
- `nind_engine.py` — `NindIndexer` (writer) and `NindEngine` (BM25-style search) built on top of the above. `NindIndexer` is the *only* Python writer for the index-family formats (`.nindlexiconindex`/`.nindtermindex`/`.nindlocalindex`) — it hand-rolls the binary layout directly via `NindFile`, replicating what `NindPadFile`/`NindIndex` do in C++, since there's no Python writer class to build on. It does not write `.nindretrolexicon` (not needed by `NindEngine`, which only supports simple, non-compound words).
- `Nind_*.py` at the package root — standalone CLI scripts (dump a document, convert corpora, search, diagnostics)
- `amose/` — Amose-specific conversion/parsing scripts (Lucene dump, XML-CLEF, sample corpora)

## Binary format verification

Binary file formats are specified in EBNF grammar (see README). The intended workflow is: C++ writes the files, Python (independently implementing the same grammar) reads/checks them — this is why the Python and C++ implementations must be kept in sync at the file-format level even though they share no code.
