#!/usr/bin/env bash
# MS MARCO v2: the archive verified, unpacked, tokenized with and without stopwords, prefixes cut.
source "$(dirname "$0")/common.sh"
set -e
D=irds/msmarco-passage-v2
tar xf $D/msmarco_v2_passage.tar -C $D
ls $D/msmarco_v2_passage | wc -l
# ir_datasets wants its docs downloaded only for iterating them; queries and qrels come on their own
time $PY scripts/prepare_v2.py $D/msmarco_v2_passage data/v2-stop --stopwords --workers 40
time $PY scripts/prepare_v2.py $D/msmarco_v2_passage data/v2 --workers 40
for s in 1000000:1M 4000000:4M 8000000:8M 16000000:16M 32000000:32M 64000000:64M; do
    $PY $R/subset.py data/v2-stop data/v2-stop-${s#*:} ${s%:*}
done
$PY $R/subset.py data/v2 data/v2-16M 16000000
du -sh data/v2*
date; echo V2 PREPARED
