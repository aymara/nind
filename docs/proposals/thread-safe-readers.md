---
orphan: true
---

# Proposal: thread-safe readers, one index shared by many search threads

Status: **proposal, not implemented** (2026-11-04). Companion to
{doc}`format-v2-blocks`; independent of it (no format change), and best done
first, since v2's block reads need the same read path.

## Why

Search on machines with hundreds of cores means many concurrent queries over
one index. Today a nind reader object can serve **one query at a time**:

- **`NindFile`** reads through one stdio `FILE*`: one file position and one
  stdio buffer, shared by every call. Each read is `flush` + `fseek` +
  `fread` into the object's single read buffer (`m_rbuffer`, `m_rPtr`), which
  the decoders then consume (`getInt4`, `getUIntLat`...).
- **`NindIndex::getDefinition`** seeks to the indirection entry, reads it,
  then seeks to the definition and reads it, into that same buffer.
- **`NindPadFile`** keeps the indirection map (`m_entriesBlocksMap`). A
  reader that misses an entry rebuilds it (`getEntryPos` →
  `mapEntriesBlocks`) to catch up with a writer appending in place: a write
  to shared state on the read path.
- **The Python layer** works around this: `NindEngine` serializes every call
  with an `RLock` ("the native readers aren't thread-safe"), and
  `Bm25Scorer`'s scratch arrays are taken with `try_lock`. A concurrent search
  allocates its own O(N) arrays (on MS MARCO, 70 MB of scores per concurrent
  query).

The workaround today is one process per search worker. That works, but every
process holds its own copy of everything per index: document lengths (a
Python dict, most of nind's 1.5 GB peak search memory on MS MARCO), the scorer's arrays, the document names.
With threads, one copy and one page cache serve all of them.

## Principles kept from the original design

- **Readers synchronize through the files themselves** (the original author's
  *synchronisation des lecteurs par les fichiers*, LAT2017.JYS.468, referenced
  by his other documents but not available here): one writer appends in
  place, readers notice new definitions lazily, and no lock is taken across
  processes. Section 2 keeps that protocol and only makes it safe between
  threads of one process; LAT2017.JYS.468 should be checked before
  implementing it.
- **Function first, the byte last**, and **a robust system accommodates
  errors**: thread safety must not cost the single-threaded path (the
  existing API is kept as is), and a reader that meets a damaged or
  half-written definition reports it (exception) and keeps serving the rest,
  as today.
- **The term index can be rebuilt from the lexicon and the local index**:
  which is what makes treating a published generation as immutable (section
  3) safe. Damage is repaired by a rebuild into a new generation, never by
  patching files under live readers.

## Proposal

Separate what's **shared and immutable** (the open file, the indirection
map, per-index tables) from what's **per call** (the read buffer and its
cursor). Make the read path `const` and free of shared mutable state, and
keep today's API working on top of it.

### 1. Positioned reads, caller-owned buffers

- **`NindFile`** gets a positioned read: `readAt(offset, bytes, ReadBuffer&)
  const`, with `pread` on POSIX (`ReadFile` with an `OVERLAPPED` offset on
  Windows) on a plain file descriptor. No shared file position, no stdio
  buffer to discard, **one syscall per read**. The I/O follow-up in CLAUDE.md
  (`flush` + two seeks + two reads per definition) goes away at the same time.
- **`ReadBuffer`** (per call, or per thread and reused): the bytes plus the
  cursor, with today's bounds-checked decoders (`getInt1..5`, `getUIntLat`,
  `getSIntLat`, `getString`) moved onto it, unchanged. The memory-safety
  conventions stay: every length or offset read from the file is bounded
  against the buffer or the real file size before use.
- **`NindIndex::getDefinition(ident, ReadBuffer&) const`**: read the
  indirection entry, then the definition, with two `readAt` calls (one, once
  the map caches the entries, see 2).
- **The decoders become `const` methods taking the buffer**:
  `NindTermIndex::getTermPostings(ident, ReadBuffer&, ...) const`,
  `getTermDef`, `NindLocalIndex::getLocalDef`,
  `NindLexiconIndex::getWordId`, and so on.

### 2. The indirection map: an immutable snapshot, swapped atomically

The map is read on every lookup and changes only when a writer appends
indirection blocks. Readers get it as a `std::shared_ptr<const EntriesMap>`,
loaded with `std::atomic_load` (C++11; the code base is C++14). A reader that
misses an entry rebuilds a new map *on the side* and publishes it with
`std::atomic_compare_exchange`. Concurrent lookups keep the snapshot they
loaded, and the read path takes no lock. That's today's lazy resynchronization,
made safe across threads.

### 3. Two kinds of files, one API

| | written in place while read (Amose: one writer, live readers) | immutable (Python `NindIndexer` generations: never modified once published) |
|---|---|---|
| reads | `pread` | `mmap` of the whole file, read-only: no syscall per read, pages shared by every thread and process |
| map | lazily resynchronized (2) | built once at open |
| consistency | as today: the writer writes data before indirection, inside its critical section; a reader sees the old or the new definition | trivially consistent |

The immutable mode is opt-in at open (`immutable=true`). `NindEngine` passes
it, because a published generation is never modified (see `_index_store.py`).
For immutable files, the `ReadBuffer` can point straight into the mapping
(zero copy). The bounds checks then run against the mapped size.

### 4. The existing API stays

The current, non-`const` methods (what Amose calls) stay and keep their
behaviour. They become thin wrappers that use a member `ReadBuffer`, so they
remain single-threaded, as today. Thread-safe callers use the new `const`
overloads. Nothing changes for code that doesn't need threads.

### 5. Python

- **`nind._native`** binds the `const` read path and releases the GIL around
  every native read (term postings, local definitions, lexicon lookups), not
  only in `Bm25Scorer.search`.
- **`Bm25Scorer`**: per-thread scratch arrays from a small pool (a free list,
  sized by the concurrency seen) instead of `try_lock` plus an O(N)
  allocation per concurrent search.
- **`NindEngine`**: the `RLock` is replaced by snapshot reference counting.
  A query takes the current `_Snapshot` and releases it when done; switching
  to a newly published generation installs the new snapshot for later
  queries, and the old one closes when its last query ends. Concurrent
  `search` calls from a `ThreadPoolExecutor` then run in parallel, sharing
  one copy of the document lengths and names.

### 6. Later: one query on many cores

With blocks ({doc}`format-v2-blocks`), one query over a large index can also
be split by document ranges. Each thread scores the blocks of its range into
its own part of the scratch arrays, and the per-range top k are merged. That
needs the read path above (concurrent block reads), so it comes after it.

## Testing

- **ThreadSanitizer**: a `gbuild.sh -s thread`-style option
  (`-fsanitize=thread`, exclusive with ASan), and the unit tests under it.
- **Stress tests (C++)**: N threads issuing random lookups (postings, local
  definitions, lexicon) on one shared reader. Every result must equal the
  single-threaded one, under TSan and ASan.
- **A concurrent in-place writer**: one writer appending terms (new
  indirection blocks included) while N reader threads look up old and new
  terms. Each read returns the old definition, the new one, or "not yet",
  never a mix or an exception. This is the Amose case.
- **Corrupt input**: the `*RobustnessTest.cpp` sweeps run through the
  `const` path too, for `pread` and for `mmap`.
- **Python**: `ThreadPoolExecutor` searches give the same results as
  sequential ones, while a new generation is published mid-run. Plus a
  benchmark in `bench/retrieval/`: QPS against the number of search threads
  (1 to all cores) for nind, Lucene and tantivy, which are both thread-safe
  readers.

## Order of work

1. `ReadBuffer` + `readAt` (`pread`), the decoders moved onto it, the existing
   API as wrappers. All tests still pass, and the I/O syscalls fall.
2. `const` lookups + the atomic indirection map. Stress tests under TSan.
3. The `mmap` mode for immutable files.
4. Python: GIL released on every read, scratch pool, snapshot reference
   counting instead of the `RLock`. Concurrency benchmark.
5. With format v2: multi-threaded scoring of one query.

## Open questions

1. **Amose**: does it read from several threads today (one reader object per
   thread?), and would it move to the `const` API?
2. **Windows**: `mmap` → `CreateFileMapping`; readers there don't lock
   generations (`msvcrt` locks are exclusive), so an immutable file can't be
   deleted while mapped. That's already the case for open files.
3. **32-bit builds**: `mmap` of files over the address space isn't possible
   there. Keep `pread` as the fallback.
