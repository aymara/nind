# Command-line tools

The `nind` package is not currently wired up with installed
`console_scripts` entry points, so none of the tools below are available as
plain commands after `pip install nind`. They are plain scripts that import
their sibling modules with absolute imports (e.g. `from NindFile import
...`, not `from nind.NindFile import ...`), so run them **from inside
`src/py/nind/`** (or with that directory - not `src/py` - on `PYTHONPATH`):

```bash
cd src/py/nind
python3 Nind_search.py ../../../indices/FRE.nindlexiconindex syntagme_nominal
```

## Corpus-level tools

These operate on a whole indexed corpus, given the path to any one of its
`.nind*` files (they derive the sibling filenames themselves).

`Nind_search.py <fichier lexiconindex> <terme cherché>`
: Interactive lookup of a term (simple or `_`-joined compound, e.g.
  `syntagme_nominal`): prints which grammatical categories it occurs in and
  how often, then prompts for a document number and shows where in that
  document the term occurs.

`Nind_dumpDocument.py <fichier nind> <n° doc>`
: Dumps one previously-indexed document back to clear text, resolving
  every term through the reverse lexicon. `<n° doc>` is the document's
  external identifier, or `last` for the most recently indexed document.
  Writes `<base>-dump-<n°>.txt`.

`Nind_trouveMotsSansOccurrence.py <fichier>`
: Finds lexicon words with zero occurrences in the indexed corpus (pure
  compound-word components, or leftovers from deleted documents). Writes
  `<base>-motsSansOccurrence.txt`.

## Per-format diagnostics

Every low-level reader module (see {doc}`api/readers`) is also a runnable
script exposing an `analyse`/dump/debug CLI for its own file format - handy
when inspecting a file's structure or chasing down a corruption. Run one
with no arguments to print its usage, for example:

```bash
cd src/py/nind
python3 NindTermindex.py FRE.nindtermindex analyse
python3 NindTermindex.py FRE.nindtermindex affiche 186201
python3 NindLexiconindex.py FRE.nindlexiconindex collision 1268512
python3 NindRetrolexicon.py FRE.nindretrolexicon lexique
```

Available for `NindFile.py`, `NindPadFile.py`, `NindIndex.py`,
`NindRetrolexicon.py`, `NindLexiconindex.py`, `NindTermindex.py` and
`NindLocalindex.py`.

## Miscellaneous

`Nind_trouveNombresPremiers.py <nombre>`
: Standalone utility, unrelated to the index formats: prints the nearest
  prime numbers immediately above and below `<nombre>`.
