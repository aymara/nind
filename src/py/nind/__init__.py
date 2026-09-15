"""nind ("nouvelle indexation") - a flat-file inverted-index module.

Pure-Python package implementing nind's binary index formats. It exists
both as a standalone, installable search frontend
(:class:`nind.nind_engine.NindEngine`, :class:`nind.nind_engine.NindIndexer`)
and, per the project's original design, as an independent reader of the
same binary formats the C++ implementation writes, to verify them against
the format's EBNF grammar - see the project README and ``CLAUDE.md`` for
the full architecture.

This package intentionally does not re-export its classes at the top
level; import from the specific module you need, e.g.::

    from nind.nind_engine import NindEngine, NindIndexer
    from nind.NindLexiconindex import NindLexiconindex
"""
