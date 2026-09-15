# nind

**nind** ("nouvelle indexation") is a flat-file inverted-index module. This
site documents the **Python package** (`nind`, in `src/py/nind/`): a
pure-Python re-implementation of nind's binary index formats, plus a
modern BM25-style search API layered on top.

```{admonition} Two implementations, one format
:class: note

nind ships two independent implementations of the same binary file
formats: a production C++ stack, and this Python package. They share no
code - the Python classes exist, per the project's original design, to
read and verify the C++-produced files against the format's EBNF grammar,
in addition to standing on their own as a lightweight, install-anywhere
search frontend.
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
