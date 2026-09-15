# Architecture

## Why French names?

nind was designed and built by a French team (LATEJCON, then adapted for
CEA LIST/DIASI/LVIC's `Amose` search engine); its class, method and
variable names are French, and this documentation keeps them exactly as
written in the code - see each class and method's docstring for what it
does in English. Two spelling quirks show up throughout the source and
this documentation quotes them as-is:

- **The "ej" transliteration.** Comments and some identifiers spell out
  accented `é`/`è` as `ej`/`eh` (e.g. `dejfinition` for *définition*,
  `ejcrit` for *écrit*), a historical workaround for tooling that didn't
  handle accents well. `lit`/`ejcrit` method pairs mean "reads"/"writes".
- **Class name = file format.** A class name tells you both its role and
  the file extension it reads: `NindLexiconindex` reads `.nindlexiconindex`,
  `NindTermindex` reads `.nindtermindex`, and so on. This convention holds
  across the Python and C++ implementations.

## Two independent implementations, one binary format

nind's binary file formats are specified in an EBNF grammar. The intended
workflow is: the production C++ library writes the files, and this Python
package - built with **no shared code** - independently reads and checks
them against the same grammar. Class comments throughout the source quote
the relevant grammar rules directly above each class, since the original
design documents (referenced by internal report codes like
`LAT2014.JYS.440`) are not in the repository.

The `nind` wheel additionally bundles `nind._native`: pybind11 bindings
over the core C++ index stack. This is a *third*, independent code path
(full read/write, unlike the read-only pure-Python classes below) - it
does not change the two-implementation cross-verification design.

## Python module layering

The pure-Python classes mirror the C++ library's layering:

```text
NindFile            binary "Latecon" number/string codec (read + write)
  |
  +-- NindPadFile    generic pad-file envelope: header, indirection
        |            block(s), "en vrac" definitions, specifics +
        |            identification trailer (read only)
        |
        +-- NindRetrolexicon    id -> word, .nindretrolexicon
        |
        +-- NindIndex           id -> (offset, length), read only
              |
              +-- NindLexiconindex   word -> id, .nindlexiconindex
              +-- NindTermindex      term id -> postings, .nindtermindex
              +-- NindLocalindex     doc id -> term positions, .nindlocalindex

nind_engine (no C++ equivalent - pure-Python frontend)
  NindIndexer   the only Python *writer* for the index-family formats;
                hand-rolls the pad-file envelope via NindFile directly,
                since there is no Python writer base class to build on.
  NindEngine    BM25 search, built on NindLexiconindex/NindTermindex/
                NindLocalindex.
```

Every reader class down to `NindLocalindex` is **read-only** - deliberately,
since the Python side's role is verification, not production writing. The
one exception is `NindIndexer`, which exists purely to make the package
useful stand-alone (e.g. in this documentation's {doc}`quickstart`)
without requiring the compiled `nind._native` extension or the C++ CLI
tools.

## Where the C++ layer fits

See the top-level project `README` and `CLAUDE.md` for the C++ library
layering (`NindBasics` / `NindRetrolexicon` / `NindLexicon` / `NindIndex` /
`NindAmose` / `NindPy`) - this documentation only covers the Python
package.
