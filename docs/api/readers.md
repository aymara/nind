# Low-level API: binary format readers

Read-only classes mirroring nind's on-disk formats, layered as described
in {doc}`../architecture`. They can open files written by either this
package's own {class}`~nind.nind_engine.NindIndexer` or the C++
implementation.

## nind.NindFile

```{eval-rst}
.. automodule:: nind.NindFile
   :no-members:

.. autofunction:: nind.NindFile.clefA
.. autofunction:: nind.NindFile.clefB
.. autofunction:: nind.NindFile.catNb2Str

.. autoclass:: nind.NindFile.NindFile
   :members:
```

## nind.NindPadFile

```{eval-rst}
.. automodule:: nind.NindPadFile
   :no-members:

.. autoclass:: nind.NindPadFile.NindPadFile
   :members:
   :show-inheritance:

.. autofunction:: nind.NindPadFile.chercheVides
.. autofunction:: nind.NindPadFile.calculeRejpartition
```

## nind.NindIndex

```{eval-rst}
.. automodule:: nind.NindIndex
   :no-members:

.. autoclass:: nind.NindIndex.NindIndex
   :members:
   :show-inheritance:
```

## nind.NindRetrolexicon

```{eval-rst}
.. automodule:: nind.NindRetrolexicon
   :no-members:

.. autoclass:: nind.NindRetrolexicon.NindRetrolexicon
   :members:
   :show-inheritance:
```

## nind.NindLexiconindex

```{eval-rst}
.. automodule:: nind.NindLexiconindex
   :no-members:

.. autoclass:: nind.NindLexiconindex.NindLexiconindex
   :members:
   :show-inheritance:
```

## nind.NindTermindex

```{eval-rst}
.. automodule:: nind.NindTermindex
   :no-members:

.. autoclass:: nind.NindTermindex.NindTermindex
   :members:
   :show-inheritance:
```

## nind.NindLocalindex

```{eval-rst}
.. automodule:: nind.NindLocalindex
   :no-members:

.. autoclass:: nind.NindLocalindex.NindLocalindex
   :members:
   :show-inheritance:
```
