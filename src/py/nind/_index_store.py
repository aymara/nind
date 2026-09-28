"""Generation-based on-disk layout that makes every index operation safe.

A nind index file must never be read while it is being written, and the
three files of an index must always come from the same build. Rather than
writing into the files readers use, every build of an index writes a new
*generation* directory, and a small pointer file is atomically replaced to
publish it::

    index_dir/
        <prefix>.CURRENT            name of the published generation (replaced atomically)
        <prefix>.lock               held by the single writer of <prefix> while it builds
        <prefix>.gen-<n>-<hex>/     one complete, never modified, index per build:
            <prefix>.nindlexiconindex, <prefix>.nindtermindex, <prefix>.nindlocalindex
            generation.lock         shared by readers while they open the files,
                                    exclusive while the generation is deleted

- A reader resolves ``CURRENT`` once, then opens every file of that
  generation under a shared lock: it gets one consistent index, however
  many rebuilds happen meanwhile. Open files stay readable after their
  generation is deleted (POSIX), so the lock is only needed while opening.
- A writer builds into a fresh generation, flushes it to disk, publishes it
  by replacing ``CURRENT``, then deletes older generations nobody is
  opening. A failed or interrupted build never touches ``CURRENT``: the
  previous index stays published, and the leftover directory is deleted by
  the next build.
- Two writers of the same index are serialized by ``<prefix>.lock``.

On Windows, file locks are exclusive only (``msvcrt``): readers don't lock,
and a generation whose files are still open can't be deleted - it is simply
retried at the next build; a reader that loses a race against a deletion
retries on the new ``CURRENT``.
"""
import contextlib
import os
import secrets
import shutil

try:
    import fcntl
except ImportError:         # Windows
    fcntl = None
    import msvcrt

__all__ = ["IndexStore", "INDEX_EXTENSIONS"]

#: Extensions of the files making up an index (``.nindretrolexicon`` only for some writers).
INDEX_EXTENSIONS = (".nindlexiconindex", ".nindretrolexicon", ".nindtermindex", ".nindlocalindex")

_GENERATION_LOCK = "generation.lock"

# File descriptors of the locks held by this process. A forked child (a worker
# pool, or any fork of user code) must not keep them: flock locks belong to the
# open file, shared with the child, so a child outliving a killed writer would
# keep the index locked forever. The parent's locks are unaffected.
_lock_fds = set()


def _close_inherited_locks():
    for fd in _lock_fds:
        try:
            os.close(fd)
        except OSError:
            pass
    _lock_fds.clear()


if hasattr(os, "register_at_fork"):
    os.register_at_fork(after_in_child=_close_inherited_locks)


class _FileLock:
    """Advisory lock on a file, released when closed (or when the process dies)."""

    def __init__(self, path, create=True):
        flags = os.O_RDWR | (os.O_CREAT if create else 0)
        self._fd = os.open(path, flags, 0o644)     # not inherited by exec'd processes (PEP 446)
        _lock_fds.add(self._fd)

    def acquire(self, shared=False, blocking=True):
        """:return: True if the lock was taken (always, when ``blocking``)."""
        if fcntl is not None:
            operation = (fcntl.LOCK_SH if shared else fcntl.LOCK_EX) | (0 if blocking else fcntl.LOCK_NB)
            try:
                fcntl.flock(self._fd, operation)
            except BlockingIOError:
                return False
            return True
        if shared:
            return True     # msvcrt has no shared locks: readers rely on retrying instead
        while True:
            try:
                msvcrt.locking(self._fd, msvcrt.LK_NBLCK, 1)
                return True
            except OSError:
                if not blocking:
                    return False
                # LK_LOCK gives up after 10 s: keep waiting like flock does
                try:
                    msvcrt.locking(self._fd, msvcrt.LK_LOCK, 1)
                    return True
                except OSError:
                    continue

    def close(self):
        if self._fd is not None:
            _lock_fds.discard(self._fd)
            os.close(self._fd)      # releases the lock
            self._fd = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _fsync_path(path, directory=False):
    # makes a file's content, or a directory's entries, durable before publishing
    if directory and os.name == "nt":
        return      # directories can't be opened for fsync on Windows
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


class IndexStore:
    """The generations of one index (``prefix``) in ``index_dir``."""

    def __init__(self, index_dir, prefix):
        self.index_dir = index_dir
        self.prefix = prefix
        self._current_path = os.path.join(index_dir, prefix + ".CURRENT")

    # -- discovery --------------------------------------------------------
    @staticmethod
    def find_prefix(index_dir):
        """The prefix of the only index in ``index_dir``.

        :raises FileNotFoundError: if there is no index in ``index_dir``.
        :raises ValueError: if there are several (pass a prefix explicitly).
        """
        names = os.listdir(index_dir)
        prefixes = {n[:-len(".CURRENT")] for n in names if n.endswith(".CURRENT")}
        if not prefixes:
            # indexes written before generations existed: flat files in index_dir
            prefixes = {n[:-len(".nindlexiconindex")] for n in names if n.endswith(".nindlexiconindex")}
        if not prefixes:
            raise FileNotFoundError("No nind index found in %s" % index_dir)
        if len(prefixes) > 1:
            raise ValueError("Several nind indexes in %s (%s): specify a prefix" % (index_dir, ", ".join(sorted(prefixes))))
        return prefixes.pop()

    # -- readers ----------------------------------------------------------
    def current_version(self):
        """A cheap token that changes whenever a new generation is published (None if none)."""
        try:
            st = os.stat(self._current_path)
        except FileNotFoundError:
            return None
        return (st.st_ino, st.st_mtime_ns, st.st_size)

    def _read_current(self):
        try:
            with open(self._current_path, "r", encoding="utf-8") as f:
                name = f.read().strip()
        except FileNotFoundError:
            return None
        # a published name is a plain directory name of this index, never a path
        if not name.startswith(self.prefix + ".gen-") or os.sep in name or (os.altsep and os.altsep in name):
            raise ValueError("Corrupt %s" % self._current_path)
        return name

    def open_current(self, opener, attempts=100):
        """Open the published generation with ``opener(base)``, consistently.

        ``opener`` receives the generation's base path (``.../<prefix>``, to
        which the index file extensions are appended) and must open every
        file it needs before returning. Indexes written before generations
        existed (flat files in ``index_dir``) are opened as they are.

        :return: ``(opener's result, version)``; ``version`` is what
            :meth:`current_version` returned when that generation was current.
        """
        for _ in range(attempts):
            version = self.current_version()
            name = self._read_current()
            if name is None:
                return opener(os.path.join(self.index_dir, self.prefix)), None
            generation = os.path.join(self.index_dir, name)
            try:
                lock = _FileLock(os.path.join(generation, _GENERATION_LOCK), create=False)
            except FileNotFoundError:
                continue        # deleted meanwhile: CURRENT names a newer generation
            with lock:
                lock.acquire(shared=True)
                # a deletion may have completed while we were waiting for the lock
                if not os.path.exists(os.path.join(generation, _GENERATION_LOCK)):
                    continue
                try:
                    return opener(os.path.join(generation, self.prefix)), version
                except Exception:
                    # only a generation lost to a race is retried; a real error is raised
                    if self._read_current() == name and os.path.exists(os.path.join(generation, _GENERATION_LOCK)):
                        raise
        raise RuntimeError("Could not open a stable generation of %s" % os.path.join(self.index_dir, self.prefix))

    # -- writer -----------------------------------------------------------
    @contextlib.contextmanager
    def new_generation(self):
        """Build a new generation and publish it if the block succeeds.

        Yields the base path to write the index files at. Waits for any other
        writer of this index. On success the files are flushed to disk, the
        generation is published, then older generations and any flat files
        of an index written before generations existed are deleted (those
        still being opened are kept for the next build). On failure the new
        generation is deleted and the published index is left untouched.
        """
        with _FileLock(os.path.join(self.index_dir, self.prefix + ".lock")) as writer_lock:
            writer_lock.acquire()
            name = "%s.gen-%06d-%s" % (self.prefix, self._next_number(), secrets.token_hex(4))
            generation = os.path.join(self.index_dir, name)
            os.mkdir(generation)
            try:
                with _FileLock(os.path.join(generation, _GENERATION_LOCK)) as lock:
                    lock.acquire()      # nobody opens it before it's published
                    yield os.path.join(generation, self.prefix)
                    for entry in os.listdir(generation):
                        if entry != _GENERATION_LOCK:
                            _fsync_path(os.path.join(generation, entry))
                    _fsync_path(generation, directory=True)
                    self._publish(name)
            except BaseException:
                shutil.rmtree(generation, ignore_errors=True)
                raise
            self._collect(keep=name)

    def _next_number(self):
        numbers = [0]
        for entry in os.listdir(self.index_dir):
            if entry.startswith(self.prefix + ".gen-"):
                try:
                    numbers.append(int(entry[len(self.prefix + ".gen-"):].split("-")[0]))
                except ValueError:
                    pass
        return max(numbers) + 1

    def _publish(self, name):
        temporary = "%s.tmp-%s" % (self._current_path, secrets.token_hex(4))
        with open(temporary, "w", encoding="utf-8") as f:
            f.write(name + "\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, self._current_path)       # atomic, on POSIX and Windows
        _fsync_path(self.index_dir, directory=True)

    def _collect(self, keep):
        # only the writer collects, holding the writer lock: no build is in progress
        for entry in os.listdir(self.index_dir):
            path = os.path.join(self.index_dir, entry)
            if entry.startswith(self.prefix + ".gen-") and entry != keep and os.path.isdir(path):
                try:
                    lock = _FileLock(os.path.join(path, _GENERATION_LOCK))
                except OSError:
                    continue
                with lock:
                    if lock.acquire(blocking=False):    # nobody is opening it
                        shutil.rmtree(path, ignore_errors=True)
            elif entry.startswith(self.prefix + ".CURRENT.tmp-"):
                _remove_quietly(path)                   # left by an interrupted publish
        for extension in INDEX_EXTENSIONS:
            _remove_quietly(os.path.join(self.index_dir, self.prefix + extension))


def _remove_quietly(path):
    try:
        os.remove(path)
    except OSError:
        pass
