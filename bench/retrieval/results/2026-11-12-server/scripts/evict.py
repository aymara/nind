#!/usr/bin/env python3
"""Evict every file under the given directories from the page cache (posix_fadvise DONTNEED;
pages mapped or dirty elsewhere stay). Then reads charged to a capped cgroup are its own."""
import os, sys
for top in sys.argv[1:]:
    for root, _, files in os.walk(os.path.realpath(top)):
        for name in files:
            fd = os.open(os.path.join(root, name), os.O_RDONLY)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            os.close(fd)
