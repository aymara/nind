# nind

**nind** ("nouvelle indexation") is a flat-file inverted-index module. This
site documents the **Python package** (`nind`, in `src/py/nind/`): an
ergonomic Python frontend to nind's binary index formats, plus a modern
BM25-style search API layered on top.

```{admonition} One format, one C++ implementation, an ergonomic Python API
:class: note

nind's binary file formats are specified in an EBNF grammar (see the
project `README`) and produced/read by a single production C++ stack. The
`nind` Python package is not a second, independent implementation of that
stack - its low-level classes are thin wrappers over `nind._native`
(pybind11 bindings compiled from that same C++ code), so reading or
writing a file in Python exercises the real C++ implementation underneath.
The only genuinely hand-rolled Python parsing left is for a handful of
diagnostic/introspection methods (`dumpeFichier`, `afficheTerme`, and
similar) that `nind._native` doesn't expose yet.
```

## Where to start

- **New to the package?** Start with {doc}`quickstart` - index a handful of
  files and run a search in a few lines.
- **Building something on the low-level formats?** {doc}`architecture`
  explains how the module layers fit together, and the {doc}`api/index`
  documents every public class and function.
- **Just want the CLI?** See {doc}`cli`.

## Installation

```bash
pip install nind
```

This installs the pure-Python `nind` package together with its compiled
`nind._native` extension (pybind11 bindings over the C++ index stack) -
see {doc}`architecture` for how the two relate. Building from source
requires a C++14 compiler; `pip`/`uv` pull in the rest (`scikit-build-core`,
`pybind11`, `cmake`, `ninja`) automatically.

```{toctree}
:hidden:

quickstart
architecture
api/index
cli
```
