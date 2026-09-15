# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

NIND ("nouvelle indexation") is a flat-file inverted-index module originally built for the `Amose` search engine (replacing the deprecated `S2` and `Lucene` indexers). It has two parallel implementations of the same binary formats:

- **C++** (`src/cpp/`): the production indexing/search libraries.
- **Python** (`src/py/nind/`): an ergonomic Python frontend to the same binary formats. Historically written as a from-scratch pure-Python re-implementation of the C++ readers/writers (no shared code between the two) simply because no Python bindings existed yet — the low-level classes are being migrated onto the `nind._native` pybind11 bindings instead (see below), keeping only their diagnostic/introspection tooling as hand-rolled parsing where `nind._native` has no equivalent.

The repo is mid-refactor (branch `refactoring-for-codecommons`): the Python package was recently moved from `src/py/*.py` into a proper `src/py/nind/` package, and a new higher-level `nind_engine.py` (BM25-style search API) is being layered on top of the low-level Python classes.

The `nind` wheel additionally bundles a compiled extension, `nind._native` (pybind11 bindings over the core C++ index stack — `NindLexiconIndex`, `NindTermIndex`, `NindLocalIndex`, `NindRetrolexicon`, `NindLexicon`; full read/write) — see `src/cpp/NindPy/`. This is the intended long-term backing for the low-level Python classes above (`NindLexiconindex`, `NindTermindex`, `NindLocalindex`, `NindRetrolexicon`, and `nind_engine.NindIndexer`'s writer): they delegate their hot-path lookups/writes to it rather than hand-rolling binary parsing, while their diagnostic-only methods (`analyseFichierXxx`, `dumpeFichier`, etc.) stay on the original pure-Python parsing since `nind._native` doesn't expose that introspection surface.

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

The Python package (`nind`, in `src/py/nind`) is managed with `pyproject.toml`/`scikit-build-core` (the old `src/py/CMakeLists.txt` build path is kept only for compatibility). `uv sync`/`uv build` compiles `nind._native` (via CMake — see `src/cpp/NindPy/CMakeLists.txt`) and installs the package (editable for `sync`) plus dev dependencies (pytest) into `.venv`. Building requires a C++14 compiler; `scikit-build-core`, `pybind11`, `cmake`, and `ninja` themselves are all pulled automatically as PEP 517 build requirements (the latter two so builds work even in containers with no system CMake/Ninja, e.g. manylinux in CI) — no manual install needed.

Tests live in `tst/py/` (one file per module, mirroring `tst/cpp/unit/`), configured via `[tool.pytest.ini_options]` in `pyproject.toml`:

```
uv sync                 # first time / after touching pyproject.toml or src/cpp/NindPy/**
uv run pytest tst/py -v
uv build                 # produce the platform wheel (nind-*.whl) + sdist in dist/
```

`src/cpp/NindPy/CMakeLists.txt` is a standalone CMake project (separate from the top-level `CMakeLists.txt`/`gbuild.sh` flow) that compiles the relevant `NindBasics`/`NindRetrolexicon`/`NindLexicon`/`NindIndex` sources directly into the `_native` extension module — not linked against the shared libraries `gbuild.sh` produces — so the wheel ships one self-contained `.so` with no internal RPATH/dlopen. It is not wired into `gbuild.sh`/ctest; touching the core C++ sources may require rebuilding both independently.

The current test suite validates the Python implementation against itself (round-trips through `nind_engine.NindIndexer`'s writers, plus hand-built fixtures for formats it doesn't write, like `.nindretrolexicon`), not against separately-produced C++ binary files — the Python and C++ implementations aren't an intentional independent cross-check of each other (see `## Binary format verification` below).

### Releasing

Version bumps are managed by `bumpver` (config: `[tool.bumpver]` in `pyproject.toml`) — it keeps `current_version` (git-tag form, `vX.Y.Z`) and the PEP 440 form in sync everywhere it appears: `[project] version` in `pyproject.toml` and every `__version__ = "..."` across `src/py/nind/`. Cutting a release is:

```
uvx bumpver update --patch    # or --minor / --major; add --dry to preview first
```

This commits, tags (`vX.Y.Z`), and *pushes* in one step (`push = true` in config) — pushing the tag is what kicks off `.github/workflows/publish-pypi.yml`, which builds wheels for Linux/macOS/Windows (via `cibuildwheel`, using `uv` as its build frontend) plus an sdist (`uv build --sdist`), and publishes them to PyPI with `uv publish --trusted-publishing always`. That workflow authenticates via PyPI Trusted Publishing (OIDC) against the `pypi` GitHub Environment — no stored token — but this only works once the `nind` PyPI project has that repo+workflow+environment registered as a trusted publisher (pypi.org project settings → Publishing); until then the `publish` job's `id-token` exchange will fail.

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

NindPy (deps: NindBasics, NindRetrolexicon, NindLexicon, NindIndex sources, compiled in directly)
   — pybind11 bindings, built only through the Python wheel (scikit-build-core), not gbuild.sh
```

All C++ code lives in namespace `latecon::nindex`. Class/file naming mirrors the binary format they implement (e.g. `NindLexiconIndex` reads/writes the `.nindlexiconindex` file format) — this naming convention holds across both the C++ and Python trees, so a class name tells you both its role and its file extension.

Design docs are referenced by internal report codes in file header comments (e.g. `LAT2014.JYS.440` = "nind, indexation post-S2", `LAT2015.JYS.448` = "Adaptation de l'indexation nind au moteur de recherche Amose") — these aren't in the repo, so treat the header comments above each class as the closest available spec.

Linker flags (`-Wl,-z,defs,--no-as-needed`) are set project-wide because these libraries are loaded dynamically via a plugin/factory mechanism, so symbols must stay visible even when not directly referenced by the linking binary — don't "clean up" apparently-unused exports without checking for this.

`tst/cpp/` mirrors the module structure for test binaries, but note `tst/cpp/CMakeLists.txt` currently only builds `NindIndex`, `NindSearch`, and `NindAmose` tests — the `NindLexicon`, `NindTests`, and `NindBasics` test subdirectories are commented out.

### Python package (`src/py/nind/`)

Parallels the C++ layering with lowercase-suffixed module names for the index classes:

- `NindFile.py` — low-level binary codec. Historically read-only-oriented (the `litXxx` getters have long existed for every format), but now has a full writer counterpart too: `ejcritNombre1/3/4/5`, `ejcritNombreULat`/`ejcritNombreSLat` (both varint encodings, full tier range matching the readers and the C++ implementation), `ejcritChaine`, `ejcritZejros`.
- `NindPadFile.py` — the shared "pad file" envelope (header, indirection block(s), specifics, identification trailer); read-only, no writer (mirrors `NindBasics::NindPadFile`, but nothing in Python subclasses it as a writer the way `NindIndex`/`NindRetrolexicon` do in C++).
- `NindRetrolexicon.py`, `NindLexiconindex.py`, `NindTermindex.py`, `NindLocalindex.py` — ergonomic read-only Python wrappers around the corresponding `NindIndex`-family C++ classes; their hot-path lookups delegate to `nind._native`, while diagnostic/analysis methods (`analyseFichierXxx`, `dumpeFichier`, etc.) remain hand-rolled pure-Python parsing, since `nind._native` doesn't expose that introspection surface. `NindLexiconindex` is hash-bucketed (`clefB(word) % nombreIndirection`), the other two are directly indexed by id.
- `nind_engine.py` — `NindIndexer` (writer) and `NindEngine` (BM25-style search) built on top of the above. `NindIndexer` is the *only* Python writer for the index-family formats (`.nindlexiconindex`/`.nindtermindex`/`.nindlocalindex`) — it builds them via `nind._native`'s `is_writer=True` constructors. It does not write `.nindretrolexicon` (not needed by `NindEngine`, which only supports simple, non-compound words).
- `Nind_*.py` at the package root — standalone CLI scripts (dump a document, convert corpora, search, diagnostics)
- `amose/` — Amose-specific conversion/parsing scripts (Lucene dump, XML-CLEF, sample corpora)

## Binary format verification

Binary file formats are specified in EBNF grammar (see README). The Python package reads/writes these formats either through `nind._native` (the compiled C++ implementation itself, via pybind11) or, for the diagnostic-only tooling not exposed by those bindings, through hand-rolled pure-Python parsing that must stay in sync with the grammar at the file-format level. There is no intentional independent-reimplementation-for-cross-verification design here — that framing in earlier notes was a misunderstanding of the project's actual intent, which was simply to give Python callers an ergonomic interface.
