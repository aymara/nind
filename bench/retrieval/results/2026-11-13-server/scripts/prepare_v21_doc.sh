#!/usr/bin/env bash
# MS MARCO v2.1 documents: unpacked, tokenized with and without stopwords (dev topics, and the judged
# TREC DL 2021-2023 topics as a second dataset sharing the corpus), prefixes cut.
source "$(dirname "$0")/common.sh"
set -e
D=raw
Q=raw/v2.1-topics-qrels
tar xf $D/msmarco_v2.1_doc.tar -C $D
ls $D/msmarco_v2.1_doc | wc -l
TREC="--trec-topics $Q/topics.dl21.txt $Q/topics.dl22.txt $Q/topics.dl23.txt
      --trec-qrels $Q/qrels.dl21-doc-msmarco-v2.1.txt $Q/qrels.dl22-doc-msmarco-v2.1.txt $Q/qrels.dl23-doc-msmarco-v2.1.txt"
DEV="--topics $Q/topics.msmarco-v2-doc.dev.txt --qrels $Q/qrels.msmarco-v2.1-doc.dev.txt"
time $PY scripts/prepare_v21_doc.py $D/msmarco_v2.1_doc data/v21doc-stop $DEV --trec-out data/v21doc-stop-trec $TREC --stopwords --workers 40
time $PY scripts/prepare_v21_doc.py $D/msmarco_v2.1_doc data/v21doc $DEV --trec-out data/v21doc-trec $TREC --workers 40
for s in 1000000:1M 4000000:4M; do
    $PY $R/subset.py data/v21doc-stop data/v21doc-stop-${s#*:} ${s%:*}
done
du -shL data/v21doc*
date; echo V21DOC PREPARED
