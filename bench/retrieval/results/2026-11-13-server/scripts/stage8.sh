#!/usr/bin/env bash
# Stage 8: MS MARCO v2.1 documents (TREC RAG 2024's corpus: 10.96M web pages, 1,191 tokens on average
# without stopwords, 13.1G tokens), nind 4.2.2 (4.2.0's code) against Lucene and tantivy, as stage 2
# did for the v2 passages. bm25s left out (exhaustive; ~14 GB at 8.8M passages).
source "$(dirname "$0")/common.sh"
W=work/v21doc
SYSTEMS=nind,nind-8s,nind-32s,lucene,tantivy
SEARCH_ONLY=nind-4t,nind-8t,nind-8s-8t,nind-32s-8t
date; idle_check
# 1: prefixes and the whole corpus, stopwords removed; dev queries (4,552), a whole pass of warm-up
for d in v21doc-stop-1M v21doc-stop-4M v21doc-stop; do
    case $d in v21doc-stop) phases=build,search,update; repeats=2 ;; *) phases=build,search; repeats=1 ;; esac
    drive --work $W --data data/$d --systems $SYSTEMS --phases $phases --repeats $repeats --warmup 4552
    drive --work $W --data data/$d --systems $SEARCH_ONLY --phases search --repeats $repeats --warmup 4552
    date
    case $d in v21doc-stop) ;; *) rm -rf $W/$d/*/index $W/$d/*/index-update ;; esac
done
# 2: stopwords kept
drive --work $W --data data/v21doc --systems nind,nind-8s,lucene,tantivy --phases build,search --warmup 4552
drive --work $W --data data/v21doc --systems nind-8t,nind-8s-8t --phases search --warmup 4552
date
# 3: the judged TREC DL 2021-2023 topics (215), on the same indexes (linked): our BM25 parameters,
# then Anserini's defaults (k1=0.9, b=0.4) in a work directory of their own
link_trec() {  # link_trec WORK DATASET SYSTEM...: WORK/DATASET-trec/S/index -> W/DATASET/S/index
    local work=$1 d=$2; shift 2
    for s in "$@"; do
        mkdir -p $work/$d-trec/$s
        [ -e $W/$d/$s/index ] && ln -sfn $PWD/$W/$d/$s/index $work/$d-trec/$s/index
    done
}
for work in $W work/v21doc-anserini; do
    link_trec $work v21doc-stop nind nind-8s nind-32s lucene tantivy
    link_trec $work v21doc nind nind-8s lucene tantivy
done
for d in v21doc-stop-trec v21doc-trec; do
    case $d in v21doc-stop-trec) s=nind,nind-8s,nind-32s,lucene,tantivy ;; *) s=nind,nind-8s,lucene,tantivy ;; esac
    drive --work $W --data data/$d --systems $s --phases search --repeats 3 --warmup 215
    drive --work work/v21doc-anserini --data data/$d --systems $s --phases search --repeats 3 --warmup 215 --k1 0.9 --b 0.4
done
date
$PY $R/driver.py --work $W --report-only > /dev/null
$PY $R/driver.py --work work/v21doc-anserini --report-only > /dev/null
# 4: every definition of every segment read back
for d in v21doc-stop v21doc; do for s in nind nind-8s; do
    echo "verify $d $s"; $SOCKET0 $PY scripts/verify.py $W/$d/$s/index > logs/verify8-$d-$s.jsonl 2>&1
    tail -1 logs/verify8-$d-$s.jsonl | cut -c1-300
done; done
date; echo STAGE8 DONE
