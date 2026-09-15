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

## One binary format, one C++ implementation, an ergonomic Python API

nind's binary file formats are specified in an EBNF grammar. Class comments
throughout the source quote the relevant grammar rules directly above each
class, since the original design documents (referenced by internal report
codes like `LAT2014.JYS.440`) are not in the repository.

The `nind` wheel bundles a compiled extension, `nind._native`: pybind11
bindings over the core C++ index stack (`NindLexiconIndex`, `NindTermIndex`,
`NindLocalIndex`, `NindRetrolexicon`, `NindLexicon`; full read/write). This
is the real backing for the low-level Python classes below - their
hot-path lookups/writes, and their shared structural-diagnostic base
(`analyseFichierPadFile`/`analyseFichierIndex`), delegate to it rather than
re-implementing the binary format in Python. The one remaining place with
hand-rolled pure-Python parsing is the per-format diagnostic/introspection
methods (`dumpeFichier`, `debogueIndex`, `donneCollisions`, `donneMax`,
`donneClef`, `afficheTerme`, `afficheDocument`, and each class's own
`analyseFichierXxx` beyond the shared base) - `nind._native` doesn't expose
that introspection surface yet (see `CLAUDE.md`'s "Planned follow-up work"
for the plan to close that gap too).

## Python module layering

The pure-Python classes mirror the C++ library's layering, but the class
body itself is mostly a thin wrapper - the class hierarchy below is where
the file-format envelope's *shared, hand-rolled* parsing still lives (used
by the per-format diagnostic methods each subclass adds), while the actual
hot-path reads/writes go straight to `nind._native`:

```text
NindFile            binary "Latecon" number/string codec (read + write) -
  |                 still used to write and to back the diagnostic-only
  |                 parsing paths below
  +-- NindPadFile    generic pad-file envelope: header, indirection
        |            block(s), "en vrac" definitions, specifics +
        |            identification trailer. analyseFichierPadFile()
        |            delegates to nind._native's analyse_pad_file().
        |
        +-- NindRetrolexicon    id -> word, .nindretrolexicon
        |                       donneMot() delegates to nind._native
        |
        +-- NindIndex           id -> (offset, length). analyseFichierIndex()
              |                 delegates to nind._native's analyse_index().
              |
              +-- NindLexiconindex   word -> id, .nindlexiconindex
              +-- NindTermindex      term id -> postings, .nindtermindex
              +-- NindLocalindex     doc id -> term positions, .nindlocalindex

nind_engine (no C++ equivalent - pure-Python frontend, native-backed)
  NindIndexer   the only Python *writer* for the index-family formats;
                builds them via nind._native's is_writer=True constructors.
  NindEngine    BM25 search, built on NindLexiconindex/NindTermindex/
                NindLocalindex.
```

None of these classes write the index-family formats themselves except
`NindIndexer` (which, like the readers, delegates to `nind._native` rather
than hand-rolling the envelope) - `nind_engine` is the whole package's only
writer path. `nind._native` is a hard runtime dependency throughout: every
class above requires the compiled extension to be built (`uv sync`), even
for read-only or diagnostic use.

## Where the C++ layer fits

See the top-level project `README` and `CLAUDE.md` for the C++ library
layering (`NindBasics` / `NindRetrolexicon` / `NindLexicon` / `NindIndex` /
`NindAmose` / `NindPy`) - this documentation only covers the Python
package.
