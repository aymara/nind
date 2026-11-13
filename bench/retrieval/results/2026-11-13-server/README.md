# Server results, 2026-11-13: long documents

Stage 8 of `bench/README.md` section 8: MS MARCO v2.1 documents (TREC RAG 2024's corpus: 10,960,555
web pages, `url title headings body` as Anserini indexes them), same machine and harness as
`2026-11-10-server`, nind 4.2.2 (4.2.0's code); stage 8b reran nind's one-index builds and what
depended on them with the fix of `1daae26` (lexicons over 16.7M words).

- `stats-*.json`: the prepared datasets (`scripts/prepare_v21_doc.py`): documents, tokens, length
  distribution, postings with a frequency over 255; `-trec`: the judged TREC DL 2021-2023 topics
  (215), sharing the corpus; `-1M`, `-4M`: prefixes.
- `v21doc-*`: `driver.py`'s results and reports, k1=1.2 b=0.75: the dev topics (4,552) on 1M, 4M and
  all documents, stopwords removed (`v21doc-stop`) and kept (`v21doc`), and the TREC topics.
- `v21doc-anserini-*`: the TREC topics with Anserini's defaults (k1=0.9, b=0.4; not tantivy, fixed
  at 1.2 and 0.75), against Anserini's published scores (`docs/reproduce/from-document-collection`
  in Anserini: nDCG@10 0.5183, 0.2991, 0.2914 for DL21, 22, 23; 0.354 over the 215 topics).
- `v21doc-smoke-report-compact.md`: the first 1M, 200 queries, before the stage.
- `verify.jsonl`: `verify_index` on every segment of the one-index and 8-segment builds.
- `stage8.log`, `stage8b.log`, `prepare.log`; `scripts/`.
