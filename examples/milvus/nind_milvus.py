"""nind as the BM25 index of a Milvus collection - proof of concept.

Milvus keeps the rows (primary key, text, vectors, scalar fields) and answers
vector searches and scalar filters; nind keeps a BM25 index of one text field
of the same rows, whose document ids *are* the collection's INT64 primary keys.
Every write goes to both, so a BM25 hit is a Milvus row with no mapping table.

Searches:

* :meth:`NindBM25Collection.bm25_search` - BM25 ranking by nind, optionally
  restricted by a Milvus filter expression and completed with Milvus fields;
* :meth:`NindBM25Collection.hybrid_search` - a Milvus vector search and a nind
  BM25 search, fused by reciprocal rank fusion (RRF), as Milvus's own hybrid
  search does with its ``RRFRanker``.

Nothing here needs Milvus's sparse vectors (2.4+) nor its BM25 function (2.5+):
it works with the Milvus 2.3 server of the ``milvus`` pip package, with Milvus
Lite and with any Milvus server.

Not transactional: Milvus is written first, nind when a write (or a
:meth:`NindBM25Collection.batch`) ends. If the nind side fails, the collection
has rows the BM25 index lacks; :meth:`NindBM25Collection.sync_from_milvus`
re-reads them.
"""

import contextlib
import os

from nind.nind_engine import NindEngine, NindIndexer


def _rrf(rankings, k=60, weights=None):
    """Reciprocal rank fusion: each list of ids gives ``weight / (k + rank)`` (rank from 1)."""
    weights = weights or [1.0] * len(rankings)
    scores = {}
    for ids, weight in zip(rankings, weights):
        for rank, doc_id in enumerate(ids, 1):
            scores[doc_id] = scores.get(doc_id, 0.0) + weight / (k + rank)
    return sorted(scores.items(), key=lambda item: -item[1])


class NindBM25Collection:
    """A Milvus collection with a nind BM25 index on one of its text fields.

    :param client: a ``pymilvus.MilvusClient``.
    :param collection: the collection's name; it must exist, with an INT64
        primary key (not ``auto_id``: nind needs the keys before Milvus writes)
        and the text field.
    :param index_dir: directory of the nind index (created if needed).
    :param text_field: the field indexed by nind.
    :param pk_field: the collection's primary key field.
    :param vector_field: the vector field :meth:`hybrid_search` searches.
    :param prefix: the nind index's prefix in ``index_dir`` (default: the
        collection's name).
    :param tokenizer: nind tokenizer options (see ``NindIndexer``), used when
        the index is created; it keeps them afterwards.
    """

    def __init__(self, client, collection, index_dir, text_field="text", pk_field="id",
                 vector_field="vector", prefix=None, tokenizer=None):
        self.client = client
        self.collection = collection
        self.text_field = text_field
        self.pk_field = pk_field
        self.vector_field = vector_field
        os.makedirs(index_dir, exist_ok=True)
        self.index_dir = index_dir
        self.prefix = prefix or collection
        self._indexer = NindIndexer(index_dir, prefix=self.prefix, tokenizer=tokenizer)
        self._engine = None
        self._update = None     # the nind update of the batch() in progress

    # -- nind -----------------------------------------------------------------
    @property
    def engine(self):
        """The ``NindEngine`` of the BM25 index (``None`` before the first write).
        It follows the index's updates by itself."""
        if self._engine is None:
            try:
                self._engine = NindEngine(self.index_dir, prefix=self.prefix)
            except FileNotFoundError:
                return None
        return self._engine

    @contextlib.contextmanager
    def _writing(self):
        if self._update is not None:
            yield self._update
        else:
            with self._indexer.update() as update:
                yield update

    @contextlib.contextmanager
    def batch(self):
        """Group writes into one nind update, published when the block ends
        (an update costs ~0.25 s to publish, whatever its size: batch small writes).
        Milvus sees each write at once, BM25 searches at the end of the block;
        if the block raises, the nind side of its writes is dropped."""
        if self._update is not None:
            yield self
            return
        with self._indexer.update() as update:
            self._update = update
            try:
                yield self
            finally:
                self._update = None

    # -- writes -----------------------------------------------------------------
    def _index_rows(self, update, rows):
        for row in rows:
            # adding an existing id replaces that document
            update.add_text(row.get(self.text_field) or "", doc_id=row[self.pk_field])

    def insert(self, rows):
        """Insert ``rows`` (dicts with the primary key and the text field) into both."""
        rows = list(rows)
        self.client.insert(self.collection, rows)
        with self._writing() as update:
            self._index_rows(update, rows)

    def upsert(self, rows):
        """Insert or replace ``rows`` in both."""
        rows = list(rows)
        self.client.upsert(self.collection, rows)
        with self._writing() as update:
            self._index_rows(update, rows)

    def delete(self, ids):
        """Delete the rows ``ids`` from both."""
        ids = list(ids)
        self.client.delete(self.collection, ids=ids)
        with self._writing() as update:
            for doc_id in ids:
                update.remove(doc_id)

    def load_jsonl(self, path, embed, row_fields=None, id_field="id", text_field="contents", batch_size=1000,
                   workers=None):
        """Bulk load a JSON Lines corpus (``{"id": ..., "contents": ...}`` per line).

        nind parses and tokenizes the file in worker processes and gives each record
        a document id (a record whose ``id`` names a document replaces it); the rows
        go to Milvus with these ids as primary keys, the record's ``id`` as the
        dynamic field ``name`` and ``embed(texts)`` as vectors.

        :param row_fields: ``row_fields(record)`` -> a dict of the row's other fields
            (every field of the schema is required: an incomplete row is refused by
            a Milvus server, and crashes Milvus Lite 2.4).
        :return: the primary keys, in the file's order.
        """
        import json
        with self.batch():
            doc_ids = self._update.add_jsonl(path, id_field=id_field, text_field=text_field, workers=workers)
            rows = {}   # pk -> row: a record replacing an earlier one of the batch replaces its row
            with open(path, encoding="utf-8") as lines:
                for doc_id, line in zip(doc_ids, (line for line in lines if line.strip())):
                    record = json.loads(line)
                    row = dict(row_fields(record)) if row_fields else {}
                    row.update({self.pk_field: doc_id, self.text_field: record[text_field],
                                "name": str(record[id_field])})
                    rows[doc_id] = row
                    if len(rows) == batch_size:
                        self._upsert_embedded(rows, embed)
                        rows = {}
            if rows:
                self._upsert_embedded(rows, embed)
        return doc_ids

    def _upsert_embedded(self, rows, embed):
        # (Milvus refuses a batch with a primary key twice)
        rows = list(rows.values())
        for row, vector in zip(rows, embed([row[self.text_field] for row in rows])):
            row[self.vector_field] = list(vector)
        self.client.upsert(self.collection, rows)

    def sync_from_milvus(self, batch_size=1000):
        """(Re)index every row of the collection in nind, in one update: attaches nind
        to an existing collection, or repairs it. (Documents nind has that the
        collection lost aren't removed.)

        :return: the number of rows read.
        """
        from pymilvus import Collection
        collection = Collection(self.collection, using=self.client._using)
        iterator = collection.query_iterator(batch_size=batch_size, output_fields=[self.pk_field, self.text_field])
        count = 0
        try:
            with self.batch():
                while True:
                    rows = iterator.next()
                    if not rows:
                        break
                    self._index_rows(self._update, rows)
                    count += len(rows)
        finally:
            iterator.close()
        return count

    # -- searches ---------------------------------------------------------------
    def _fetch(self, ids, filter=None, output_fields=None):
        # the rows ids that pass filter: {id: row}
        if not ids:
            return {}
        expression = "%s in [%s]" % (self.pk_field, ", ".join(str(int(doc_id)) for doc_id in ids))
        if filter:
            expression += " and (%s)" % filter
        fields = [self.pk_field] + [field for field in output_fields or () if field != self.pk_field]
        rows = self.client.query(self.collection, filter=expression, output_fields=fields)
        return {row[self.pk_field]: row for row in rows}

    def bm25_search(self, query, limit=10, filter=None, output_fields=None, k1=1.5, b=0.75, overfetch=4):
        """The ``limit`` best rows for ``query`` by BM25 (nind).

        :param filter: a Milvus boolean expression on the rows (``category == "x"``):
            nind's ranking is read further down until ``limit`` rows pass it
            (``overfetch`` times more at a time).
        :param output_fields: fields of the rows to return, read from Milvus.
        :return: a list of dicts ``{"id": pk, "score": bm25, field: value...}``,
            best first. The scores are BM25 over the whole collection: a filter
            selects rows, it doesn't change their statistics.
        """
        engine = self.engine
        if engine is None or limit <= 0:
            return []
        if not filter and not output_fields:
            return [{"id": doc_id, "score": score} for doc_id, score in engine.search(query, top_k=limit, k1=k1, b=b)]
        wanted = limit * (overfetch if filter else 1)
        while True:
            hits = engine.search(query, top_k=wanted, k1=k1, b=b)
            rows = self._fetch([doc_id for doc_id, _ in hits], filter, output_fields)
            results = [dict(rows[doc_id], id=doc_id, score=score) for doc_id, score in hits if doc_id in rows]
            if len(results) >= limit or len(hits) < wanted:
                break
            wanted *= overfetch      # not enough rows passed the filter: read further down
        for result in results:
            if self.pk_field != "id":
                result.pop(self.pk_field, None)
        return results[:limit]

    def vector_search(self, vector, limit=10, filter=None, output_fields=None, search_params=None):
        """The ``limit`` nearest rows to ``vector`` (Milvus): dicts ``{"id", "score", fields...}``."""
        hits = self.client.search(self.collection, [list(vector)], limit=limit, filter=filter or "",
                                  output_fields=output_fields or [], search_params=search_params or {},
                                  anns_field=self.vector_field)[0]
        return [dict(hit.get("entity", {}), id=hit["id"], score=hit["distance"]) for hit in hits]

    def hybrid_search(self, query, vector, limit=10, filter=None, output_fields=None, candidates=None,
                      rrf_k=60, weights=(1.0, 1.0), search_params=None):
        """Hybrid search: nind's BM25 ranking for ``query`` and Milvus's vector ranking for
        ``vector`` (``candidates`` each, default ``5 * limit``), fused by RRF.

        :param weights: (BM25, vector) weights of the two rankings in the fusion.
        :return: a list of dicts ``{"id", "score" (RRF), "bm25_rank", "vector_rank",
            fields...}`` (a rank is ``None`` when the row isn't in that list), best first.
        """
        candidates = candidates or 5 * limit
        lexical = [hit["id"] for hit in self.bm25_search(query, candidates, filter=filter)]
        semantic = [hit["id"] for hit in self.vector_search(vector, candidates, filter=filter,
                                                             search_params=search_params)]
        fused = _rrf([lexical, semantic], k=rrf_k, weights=list(weights))[:limit]
        rows = self._fetch([doc_id for doc_id, _ in fused], None, output_fields) if output_fields else {}
        lexical_rank = {doc_id: rank for rank, doc_id in enumerate(lexical, 1)}
        semantic_rank = {doc_id: rank for rank, doc_id in enumerate(semantic, 1)}
        results = []
        for doc_id, score in fused:
            result = dict(rows.get(doc_id, {}))
            result.pop(self.pk_field, None)
            result.update(id=doc_id, score=score, bm25_rank=lexical_rank.get(doc_id),
                          vector_rank=semantic_rank.get(doc_id))
            results.append(result)
        return results
