"""nind in place of Milvus's built-in BM25 (full text search) - proof of concept.

:class:`NindMilvusClient` is a ``pymilvus.MilvusClient`` whose full text search
is done by nind instead of Milvus's BM25 function: code written for Milvus's
full text search runs unchanged, only the client's class changes::

    schema.add_field("text", DataType.VARCHAR, max_length=65535, enable_analyzer=True)
    schema.add_field("sparse", DataType.SPARSE_FLOAT_VECTOR)
    schema.add_function(Function(name="bm25", function_type=FunctionType.BM25,
                                 input_field_names=["text"], output_field_names=["sparse"]))
    index_params.add_index(field_name="sparse", index_type="SPARSE_INVERTED_INDEX", metric_type="BM25")
    client = NindMilvusClient(uri="http://localhost:19530", nind_dir="nind-indexes")
    client.create_collection("docs", schema=schema, index_params=index_params)
    client.insert("docs", [{"id": 1, "text": "..."}])
    client.search("docs", data=["a query"], anns_field="sparse", limit=10, output_fields=["text"])

What changes underneath:

* ``create_collection`` (and ``create_index``) take the BM25 functions out of
  the schema: each function's output field stays, as a nullable sparse vector
  that nothing fills (Milvus needs a vector field to load a collection), and
  nind indexes the function's input field, under ``nind_dir``.
* ``insert``/``upsert``/``delete`` write Milvus, then nind (one nind update
  per call, or per :meth:`NindMilvusClient.bm25_batch`). nind's document ids
  are the primary keys (INT64), or its document names (VARCHAR keys); with
  ``auto_id``, the ids Milvus returns.
* ``search`` on a BM25 output field is answered by nind (block-max MaxScore):
  a ``filter`` is applied by Milvus to nind's ranking (read further down until
  enough rows pass), ``output_fields`` are read from Milvus. ``hybrid_search``
  mixing it with other vector fields fuses the lists client-side, as Milvus's
  ``RRFRanker``/``WeightedRanker`` do.
* ``drop_collection``/``rename_collection`` follow on nind's side.

BM25 is Milvus's: the same idf (``log(1 + (N - df + 0.5) / (df + 0.5))``), its
``bm25_k1``/``bm25_b`` index parameters (defaults 1.2 and 0.75). The analyzer
is approximated by nind's tokenizer (see :func:`tokenizer_options`).

Not transactional (Milvus is written first): :meth:`NindMilvusClient.rebuild_bm25`
rebuilds a field's nind index from the collection's rows.
"""

import contextlib
import copy
import json
import math
import os
import shutil
import uuid

from pymilvus import DataType, FieldSchema, FunctionType, MilvusClient
from pymilvus.client.search_result import Hit

from nind.nind_engine import NindEngine, NindIndexer

#: Milvus's default BM25 parameters
BM25_K1, BM25_B = 1.2, 0.75

#: nind's tokenizer as Milvus's "standard" analyzer (runs of letters and digits, lowercased):
#: err_4012 -> err 4012, don't -> don t, utf8 and x2y whole (checked with Milvus's run_analyzer).
#: nind lowercases ASCII only: the client lowercases texts and queries itself (str.lower).
STANDARD_TOKENIZER = {"split_camel_case": False, "split_underscore": True, "split_digits": False,
                      "lowercase": True}

_PREFIX = "bm25"            # each field's nind index: <nind_dir>/<db>/<collection>/<field dir>/bm25.*
_META = "fulltext.json"
_ID_CHUNK = 10000           # ids per "pk in [...]" expression


def tokenizer_options(analyzer_params):
    """nind tokenizer options for a Milvus analyzer (``analyzer_params`` of a VARCHAR field).

    Supported: the default (``standard``), and the ``standard`` or ``whitespace``
    tokenizers with at most the ``lowercase`` filter; others (stemming, stop
    words, languages) have no nind equivalent: ``NotImplementedError`` (give
    ``NindMilvusClient(tokenizer=...)`` instead).
    """
    if isinstance(analyzer_params, str):
        analyzer_params = json.loads(analyzer_params)
    if not analyzer_params or analyzer_params == {"type": "standard"}:
        return dict(STANDARD_TOKENIZER)
    tokenizer = analyzer_params.get("tokenizer")
    filters = analyzer_params.get("filter", [])
    if set(analyzer_params) <= {"tokenizer", "filter"} and tokenizer in ("standard", "whitespace") \
            and all(name == "lowercase" for name in filters):
        return dict(STANDARD_TOKENIZER, lowercase="lowercase" in filters)
    raise NotImplementedError("analyzer %r has no nind equivalent: give NindMilvusClient(tokenizer=...)"
                              % (analyzer_params,))


def _bm25_functions(schema):
    return [function for function in (schema.functions or []) if function.type == FunctionType.BM25]


class _Field:
    """One BM25 function of a collection: its nind index."""

    def __init__(self, collection_dir, meta):
        self.input = meta["input"]
        self.output = meta["output"]
        self.k1 = meta["k1"]
        self.b = meta["b"]
        self.tokenizer = meta["tokenizer"]
        self.lowercase = bool(self.tokenizer.get("lowercase"))
        self.index_dir = os.path.join(collection_dir, meta["dir"])
        os.makedirs(self.index_dir, exist_ok=True)
        self.indexer = NindIndexer(self.index_dir, prefix=_PREFIX, tokenizer=self.tokenizer)
        self._engine = None

    def prepare(self, text):
        # (Unicode lowercasing, as Milvus's lowercase filter: nind's tokenizer does ASCII only)
        text = text or ""
        return text.lower() if self.lowercase else text

    @property
    def engine(self):
        """The field's ``NindEngine`` (``None`` before its first document); it follows updates by itself."""
        if self._engine is None:
            try:
                self._engine = NindEngine(self.index_dir, prefix=_PREFIX)
            except FileNotFoundError:
                return None
        return self._engine


class _FullText:
    """The nind side of a collection: its BM25 fields, its primary key."""

    def __init__(self, collection_dir, meta):
        self.dir = collection_dir
        self.meta = meta
        self.pk = meta["pk"]
        self.varchar_pk = meta["varchar_pk"]
        self.fields = {field["output"]: _Field(collection_dir, field) for field in meta["fields"]}

    def save(self):
        with open(os.path.join(self.dir, _META + ".tmp"), "w") as out:
            json.dump(self.meta, out, indent=1)
        os.replace(os.path.join(self.dir, _META + ".tmp"), os.path.join(self.dir, _META))

    # nind's document ids: INT64 keys modulo 2**64 (negative ones too), VARCHAR keys as names
    @staticmethod
    def doc_id(pk):
        return int(pk) % (1 << 64)

    @staticmethod
    def pk_of(doc_id):
        return doc_id - (1 << 64) if doc_id >= 1 << 63 else doc_id

    def add(self, update, field, pks, texts):
        # one native tokenizer batch (IndexUpdate.add_texts): an existing document is replaced
        texts = [field.prepare(text) for text in texts]
        if self.varchar_pk:
            update.add_texts(texts, names=list(pks))
        else:
            update.add_texts(texts, doc_ids=[self.doc_id(pk) for pk in pks])

    def remove(self, update, pk):
        if self.varchar_pk:
            doc_id = update.doc_id(pk)
            if doc_id is not None:
                update.remove(doc_id)
        else:
            update.remove(self.doc_id(pk))

    def search(self, field, text, top_k, k1, b):
        engine = field.engine
        if engine is None or top_k <= 0:
            return []
        text = field.prepare(text)
        if self.varchar_pk:
            return engine.search(text, top_k=top_k, doc_names=True, k1=k1, b=b)
        return [(self.pk_of(doc_id), score) for doc_id, score in engine.search(text, top_k=top_k, k1=k1, b=b)]


class NindMilvusClient(MilvusClient):
    """A ``MilvusClient`` whose BM25 full text search is done by nind.

    :param nind_dir: where the nind indexes go (one directory per database,
        collection and BM25 function), with their description of the
        collections; clients of one Milvus must share it.
    :param tokenizer: nind tokenizer options for every BM25 field (default:
        derived from the field's analyzer, see :func:`tokenizer_options`).
    Other arguments: ``MilvusClient``'s.
    """

    def __init__(self, uri="http://localhost:19530", *args, nind_dir, tokenizer=None, **kwargs):
        super().__init__(uri, *args, **kwargs)
        self._nind_dir = os.path.abspath(nind_dir)
        self._db = kwargs.get("db_name") or "default"
        self._tokenizer = tokenizer
        # Milvus Lite refuses a missing (null) sparse vector: the placeholder gets a dummy one
        self._placeholder = None if uri.startswith(("http:", "https:", "tcp:", "grpc:")) else {0: 1.0}
        self._collections = {}      # name -> _FullText, or None (no BM25 function)
        self._updates = None        # (collection, field) -> IndexUpdate, in a bm25_batch()
        self._batch_stack = None

    # -- the nind side of collections -------------------------------------------
    def _collection_dir(self, collection_name):
        return os.path.join(self._nind_dir, self._db, collection_name)

    def _fulltext(self, collection_name):
        if collection_name not in self._collections:
            path = os.path.join(self._collection_dir(collection_name), _META)
            fulltext = None
            if os.path.exists(path):
                with open(path) as meta:
                    fulltext = _FullText(self._collection_dir(collection_name), json.load(meta))
            self._collections[collection_name] = fulltext
        return self._collections[collection_name]

    def _forget(self, collection_name):
        self._collections.pop(collection_name, None)
        shutil.rmtree(self._collection_dir(collection_name), ignore_errors=True)

    @contextlib.contextmanager
    def _updating(self, collection_name, field):
        if self._updates is not None:
            key = (collection_name, field.output)
            if key not in self._updates:
                self._updates[key] = self._batch_stack.enter_context(field.indexer.update())
            yield self._updates[key]
        else:
            with field.indexer.update() as update:
                yield update

    @contextlib.contextmanager
    def bm25_batch(self):
        """Group writes into one nind update per BM25 field, published when the block ends
        (an update costs ~0.25 s to publish whatever its size: batch small writes).
        Milvus sees each write at once, BM25 searches at the end of the block; if the
        block raises, the nind side of its writes is dropped."""
        if self._updates is not None:
            yield self
            return
        with contextlib.ExitStack() as stack:
            self._updates, self._batch_stack = {}, stack
            try:
                yield self
            finally:
                self._updates, self._batch_stack = None, None

    # -- collections ------------------------------------------------------------
    def create_collection(self, collection_name, dimension=None, *args, schema=None, index_params=None, **kwargs):
        if self.has_collection(collection_name):
            # Milvus decides (an error, or nothing for the same schema): the nind indexes stay
            return super().create_collection(collection_name, dimension, *args, schema=schema,
                                             index_params=index_params, **kwargs)
        self._forget(collection_name)       # the leftovers of a collection dropped by another client
        functions = _bm25_functions(schema) if schema is not None else []
        if not functions:
            return super().create_collection(collection_name, dimension, *args, schema=schema,
                                             index_params=index_params, **kwargs)
        schema = copy.deepcopy(schema)
        index_params = copy.deepcopy(index_params)
        fields_meta = []
        by_name = {field.name: field for field in schema.fields}
        for function in functions:
            source, output = function.input_field_names[0], function.output_field_names[0]
            tokenizer = self._tokenizer or tokenizer_options(by_name[source].params.get("analyzer_params"))
            fields_meta.append({"input": source, "output": output, "k1": BM25_K1, "b": BM25_B,
                                "tokenizer": tokenizer, "dir": "%s-%s" % (output, uuid.uuid4().hex[:8])})
            # the output field stays, as a sparse vector nothing fills
            schema._fields[schema.fields.index(by_name[output])] = FieldSchema(
                output, DataType.SPARSE_FLOAT_VECTOR, nullable=True, description="nind BM25: %s" % source)
        schema._functions = [function for function in schema.functions if function.type != FunctionType.BM25]
        if index_params is not None:
            self._take_bm25_indexes(index_params, fields_meta)
        primary = schema.primary_field
        meta = {"pk": primary.name, "varchar_pk": primary.dtype == DataType.VARCHAR, "fields": fields_meta}
        os.makedirs(self._collection_dir(collection_name))
        fulltext = _FullText(self._collection_dir(collection_name), meta)
        fulltext.save()
        try:
            result = super().create_collection(collection_name, dimension, *args, schema=schema,
                                               index_params=index_params, **kwargs)
        except BaseException:
            self._forget(collection_name)
            raise
        self._collections[collection_name] = fulltext
        return result

    @staticmethod
    def _take_bm25_indexes(index_params, fields_meta):
        # BM25 indexes of the outputs: their k1 and b for nind, an IP index of the placeholder for Milvus
        for field in fields_meta:
            for index in index_params:
                if index._field_name == field["output"]:
                    configs = dict(index._configs)
                    params = configs.pop("params", {}) or {}
                    for key in ("k1", "b"):
                        value = params.get("bm25_" + key, configs.pop("bm25_" + key, None))
                        if value is not None:
                            field[key] = float(value)
                    index._index_type = "SPARSE_INVERTED_INDEX"
                    index._configs = {"metric_type": "IP"}

    def create_index(self, collection_name, index_params, timeout=None, **kwargs):
        fulltext = self._fulltext(collection_name)
        if fulltext is not None:
            index_params = copy.deepcopy(index_params)
            before = json.dumps(fulltext.meta["fields"], sort_keys=True)
            self._take_bm25_indexes(index_params, fulltext.meta["fields"])
            if json.dumps(fulltext.meta["fields"], sort_keys=True) != before:
                fulltext.save()
                self._collections.pop(collection_name)
        return super().create_index(collection_name, index_params, timeout=timeout, **kwargs)

    def drop_collection(self, collection_name, timeout=None, **kwargs):
        result = super().drop_collection(collection_name, timeout=timeout, **kwargs)
        self._forget(collection_name)
        return result

    def rename_collection(self, old_name, new_name, target_db="", timeout=None, **kwargs):
        result = super().rename_collection(old_name, new_name, target_db, timeout=timeout, **kwargs)
        if os.path.isdir(self._collection_dir(old_name)):
            target = os.path.join(self._nind_dir, target_db or self._db, new_name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            os.rename(self._collection_dir(old_name), target)
        self._collections.pop(old_name, None)
        self._collections.pop(new_name, None)
        return result

    # -- writes -------------------------------------------------------------------
    def _rows(self, fulltext, data):
        rows = [data] if isinstance(data, dict) else list(data)
        if self._placeholder is not None:
            rows = [dict(row, **{output: self._placeholder for output in fulltext.fields if output not in row})
                    for row in rows]
        return rows

    def _index(self, collection_name, fulltext, rows, ids, partial=False):
        for field in fulltext.fields.values():
            # (a partial update without the text leaves it as it is)
            selected = [(pk, row.get(field.input)) for row, pk in zip(rows, ids) if not partial or field.input in row]
            if selected:
                with self._updating(collection_name, field) as update:
                    fulltext.add(update, field, [pk for pk, _ in selected], [text for _, text in selected])

    def insert(self, collection_name, data, timeout=None, partition_name="", **kwargs):
        fulltext = self._fulltext(collection_name)
        if fulltext is None:
            return super().insert(collection_name, data, timeout, partition_name, **kwargs)
        rows = self._rows(fulltext, data)
        result = super().insert(collection_name, rows, timeout, partition_name, **kwargs)
        self._index(collection_name, fulltext, rows, result["ids"])     # (auto_id: Milvus's ids)
        return result

    def upsert(self, collection_name, data, timeout=None, partition_name="", **kwargs):
        fulltext = self._fulltext(collection_name)
        if fulltext is None:
            return super().upsert(collection_name, data, timeout, partition_name, **kwargs)
        rows = self._rows(fulltext, data)
        result = super().upsert(collection_name, rows, timeout, partition_name, **kwargs)
        ids = result.get("ids") or [row[fulltext.pk] for row in rows]
        self._index(collection_name, fulltext, rows, ids, partial=kwargs.get("partial_update", False))
        return result

    def delete(self, collection_name, ids=None, timeout=None, filter=None, partition_name=None, **kwargs):
        fulltext = self._fulltext(collection_name)
        if fulltext is None:
            return super().delete(collection_name, ids, timeout, filter, partition_name, **kwargs)
        if filter:
            # the rows the filter selects, deleted by id: nind removes the same ones
            iterator = self.query_iterator(collection_name, batch_size=_ID_CHUNK, filter=filter,
                                           output_fields=[fulltext.pk],
                                           partition_names=[partition_name] if partition_name else None)
            ids = []
            try:
                while True:
                    rows = iterator.next()
                    if not rows:
                        break
                    ids.extend(row[fulltext.pk] for row in rows)
            finally:
                iterator.close()
        elif not isinstance(ids, (list, tuple)):
            ids = [] if ids is None else [ids]
        count = 0
        for start in range(0, len(ids), _ID_CHUNK):
            result = super().delete(collection_name, ids[start:start + _ID_CHUNK], timeout,
                                    partition_name=partition_name, **kwargs)
            # (Milvus Lite returns the deleted keys, as Milvus before 2.3.2)
            count += result["delete_count"] if isinstance(result, dict) else len(result)
        for field in fulltext.fields.values():
            with self._updating(collection_name, field) as update:
                for pk in ids:
                    fulltext.remove(update, pk)
        return {"delete_count": count}

    def rebuild_bm25(self, collection_name, batch_size=5000):
        """Rebuild the collection's nind indexes from its rows (after a failure between
        Milvus and nind, or to attach nind to rows written by another client).

        :return: the number of rows indexed.
        """
        fulltext = self._fulltext(collection_name)
        if fulltext is None:
            raise ValueError("%s has no BM25 function" % collection_name)
        old_dirs = []
        for field in fulltext.meta["fields"]:
            old_dirs.append(field["dir"])
            field["dir"] = "%s-%s" % (field["output"], uuid.uuid4().hex[:8])
        rebuilt = _FullText(fulltext.dir, fulltext.meta)
        sources = sorted({field.input for field in rebuilt.fields.values()})
        iterator = self.query_iterator(collection_name, batch_size=batch_size, output_fields=[rebuilt.pk] + sources)
        count = 0
        with contextlib.ExitStack() as stack:
            updates = {output: stack.enter_context(field.indexer.update()) for output, field in rebuilt.fields.items()}
            try:
                while True:
                    rows = iterator.next()
                    if not rows:
                        break
                    for output, field in rebuilt.fields.items():
                        rebuilt.add(updates[output], field, [row[rebuilt.pk] for row in rows],
                                    [row.get(field.input) for row in rows])
                    count += len(rows)
            finally:
                iterator.close()
        rebuilt.save()
        self._collections[collection_name] = rebuilt
        for old in old_dirs:
            shutil.rmtree(os.path.join(fulltext.dir, old), ignore_errors=True)
        return count

    # -- searches -------------------------------------------------------------------
    def _bm25_field(self, fulltext, anns_field, data):
        if fulltext is None:
            return None
        if anns_field is None:
            if len(fulltext.fields) == 1 and data and all(isinstance(text, str) for text in data):
                return next(iter(fulltext.fields.values()))
            return None
        return fulltext.fields.get(anns_field)

    def _fetch(self, collection_name, fulltext, ids, filter, output_fields, **kwargs):
        # {pk: row} of the rows ids that pass filter, with output_fields
        rows = {}
        fields = [fulltext.pk] + [field for field in output_fields or () if field != fulltext.pk]
        for start in range(0, len(ids), _ID_CHUNK):
            chunk = ids[start:start + _ID_CHUNK]
            expression = "%s in %s" % (fulltext.pk, json.dumps(list(chunk)))
            if filter:
                expression += " and (%s)" % filter
            for row in self.query(collection_name, filter=expression, output_fields=fields, **kwargs):
                rows[row[fulltext.pk]] = row
        return rows

    def _ranked(self, collection_name, fulltext, field, text, limit, filter, k1, b, partition_names, kwargs):
        # nind's ranking of text, of rows passing filter: [(pk, score)]
        if not filter and not partition_names:
            return fulltext.search(field, text, limit, k1, b)
        wanted = limit * 4
        while True:
            hits = fulltext.search(field, text, wanted, k1, b)
            passing = self._fetch(collection_name, fulltext, [pk for pk, _ in hits], filter, None,
                                  partition_names=partition_names, **kwargs)
            results = [(pk, score) for pk, score in hits if pk in passing]
            if len(results) >= limit or len(hits) < wanted:
                return results[:limit]
            wanted *= 4         # not enough rows passed the filter: read further down

    def _hits(self, collection_name, fulltext, ranked_lists, output_fields, kwargs):
        # [[(pk, score)]] -> [[Hit]], the output fields read from Milvus at once
        rows = {}
        if output_fields:
            ids = list(dict.fromkeys(pk for ranked in ranked_lists for pk, _ in ranked))
            rows = self._fetch(collection_name, fulltext, ids, None, output_fields, **kwargs)
        results = []
        for ranked in ranked_lists:
            hits = []
            for pk, score in ranked:
                entity = {name: value for name, value in rows.get(pk, {}).items()
                          if name != fulltext.pk and name not in fulltext.fields}
                hits.append(Hit({fulltext.pk: pk, "distance": score, "entity": entity}, pk_name=fulltext.pk))
            results.append(hits)
        return results

    @staticmethod
    def _query_kwargs(kwargs):
        return {key: kwargs[key] for key in ("consistency_level", "filter_params") if key in kwargs}

    def search(self, collection_name, data=None, filter="", limit=10, output_fields=None, search_params=None,
               timeout=None, partition_names=None, anns_field=None, **kwargs):
        fulltext = self._fulltext(collection_name)
        field = self._bm25_field(fulltext, anns_field, data)
        if field is None:
            return super().search(collection_name, data, filter, limit, output_fields, search_params, timeout,
                                  partition_names, anns_field, **kwargs)
        if any(kwargs.get(key) is not None for key in ("ranker", "function_chains", "highlighter", "ids",
                                                       "search_aggregation", "group_by_field")):
            raise NotImplementedError("nind BM25 search: ranker, function chains, highlighter, ids, "
                                      "aggregation and grouping aren't supported")
        params = dict((search_params or {}).get("params") or {})
        offset = kwargs.get("offset", (search_params or {}).get("offset", 0)) or 0
        k1 = float(params.get("bm25_k1", field.k1))
        b = float(params.get("bm25_b", field.b))
        query_kwargs = self._query_kwargs(kwargs)
        ranked = [self._ranked(collection_name, fulltext, field, text, limit + offset, filter, k1, b,
                               partition_names, query_kwargs)[offset:]
                  for text in data]
        return self._hits(collection_name, fulltext, ranked, output_fields, query_kwargs)

    def hybrid_search(self, collection_name, reqs, ranker=None, limit=10, output_fields=None, timeout=None,
                      partition_names=None, **kwargs):
        fulltext = self._fulltext(collection_name)
        if fulltext is None or not any(req.anns_field in fulltext.fields for req in reqs):
            return super().hybrid_search(collection_name, reqs, ranker, limit, output_fields, timeout,
                                         partition_names, **kwargs)
        query_kwargs = self._query_kwargs(kwargs)
        lists, metrics = [], []     # per request: per query, [(pk, score)]
        for req in reqs:
            field = fulltext.fields.get(req.anns_field)
            if field is not None:
                params = dict((req.param or {}).get("params") or {})
                k1, b = float(params.get("bm25_k1", field.k1)), float(params.get("bm25_b", field.b))
                lists.append([self._ranked(collection_name, fulltext, field, text, req.limit, req.expr, k1, b,
                                           partition_names, query_kwargs) for text in req.data])
                metrics.append("BM25")
            else:
                found = super().search(collection_name, req.data, req.expr or "", req.limit, None, req.param,
                                       timeout, partition_names, req.anns_field, **kwargs)
                lists.append([[(hit.id, hit.distance) for hit in hits] for hits in found])
                metrics.append(self._metric(collection_name, req))
        fuse = self._fusion(ranker, metrics)
        offset = kwargs.get("offset", 0) or 0
        fused = [fuse([per_request[query] for per_request in lists])[offset:offset + limit]
                 for query in range(len(reqs[0].data))]
        return self._hits(collection_name, fulltext, fused, output_fields, query_kwargs)

    def _metric(self, collection_name, req):
        metric = (req.param or {}).get("metric_type")
        if metric:
            return metric.upper()
        for name in self.list_indexes(collection_name, field_name=req.anns_field):
            return self.describe_index(collection_name, name)["metric_type"].upper()
        return "COSINE"

    @staticmethod
    def _fusion(ranker, metrics):
        # Milvus's RRFRanker and WeightedRanker (and their rerank Functions), client-side
        strategy, params = "rrf", {"k": 60}
        if ranker is not None and hasattr(ranker, "_strategy"):
            strategy = ranker._strategy
            params = {"k": getattr(ranker, "_k", 60), "weights": getattr(ranker, "_weights", None),
                      "norm_score": getattr(ranker, "_norm_score", False)}
        elif ranker is not None:
            params = dict(getattr(ranker, "params", {}) or {})
            strategy = params.get("reranker", "rrf")
        if strategy == "rrf":
            k = float(params.get("k", 60))

            def fuse(rankings):
                scores = {}
                for ranking in rankings:
                    for rank, (pk, _) in enumerate(ranking, 1):
                        scores[pk] = scores.get(pk, 0.0) + 1.0 / (k + rank)
                return sorted(scores.items(), key=lambda item: -item[1])
            return fuse
        if strategy == "weighted":
            weights = list(params.get("weights") or [])
            if len(weights) != len(metrics):
                raise ValueError("WeightedRanker: %d weights for %d requests" % (len(weights), len(metrics)))
            norm = bool(params.get("norm_score", False))
            normalize = {"COSINE": lambda x: (1 + x) * 0.5, "IP": lambda x: 0.5 + math.atan(x) / math.pi,
                         "BM25": lambda x: 2 * math.atan(x) / math.pi}

            def fuse(rankings):
                scores = {}
                for ranking, weight, metric in zip(rankings, weights, metrics):
                    convert = normalize.get(metric, lambda x: 1.0 - 2 * math.atan(x) / math.pi) if norm else float
                    for pk, score in ranking:
                        scores[pk] = scores.get(pk, 0.0) + weight * convert(score)
                return sorted(scores.items(), key=lambda item: -item[1])
            return fuse
        raise NotImplementedError("ranker %r isn't supported with nind BM25" % (strategy,))
