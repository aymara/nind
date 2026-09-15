# API reference

```{toctree}
:maxdepth: 2

engine
readers
```

The reference is split in two, matching the layering described in
{doc}`../architecture`:

- {doc}`engine` - the high-level, pure-Python search frontend
  ({class}`~nind.nind_engine.NindEngine`, {class}`~nind.nind_engine.NindIndexer`).
  Start here for indexing and searching a corpus.
- {doc}`readers` - the low-level, read-only binary format readers
  ({mod}`~nind.NindFile` through {mod}`~nind.NindLocalindex`). Reach for
  these when working directly with nind's on-disk formats, including
  files written by the C++ implementation.
