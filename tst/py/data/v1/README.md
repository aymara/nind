# Format v1 indexes written by `NindIndexer`

Two indexes, `with-local` and `without-local` (`local_index=False`), written by
the `NindIndexer` of nind before format v2 (`master` at the time). Each holds the
eight `V1_TEXTS` of `tst/py/test_nind_engine.py`, named `n0`..`n7`. Their
`.ninddoclengths` and `.ninddocnames` files are the 4-byte-id versions,
`NINDDL01` and `NINDDN01`.

`test_a_format_v1_index_is_read_then_migrated_by_its_first_update` checks two
things. First, `NindEngine` reads these indexes as they are. Second, the first
`IndexUpdate` rebuilds them in format v2 before applying its changes.

They were produced with `make.py`, run by a nind built from a pre-v2 commit:

```
python make.py OUT    # then copy OUT/with-local and OUT/without-local here
```

Don't regenerate them with a current nind: it writes format v2.
