#!/usr/bin/env bash
# Stage 6: several callers at once, every system (concurrency.py): threads sharing one searcher, or
# processes each opening the index; MS MARCO v1 8.8M and v2 138M, stopwords removed, top 10.
# Pinning as in stage 3: up to 12 callers on socket 0's 12 cores, 24 on cores 0-23, 48 unpinned.
source "$(dirname "$0")/common.sh"
OUT=work/s6/concurrency.jsonl
mkdir -p work/s6 logs/s6
pin() { case $1 in 1|2|4|8|12) echo "numactl --physcpubind=0-11 --preferred=0" ;; 24) echo "numactl --physcpubind=0-23" ;; *) echo "" ;; esac; }
run() {     # run DATASET_DIR QUERIES COUNT WARMUP SYSTEM MODE
    local index=$1/$5/index
    find -L $index -type f -exec cat {} + > /dev/null      # the index in the page cache
    for c in 1 2 4 8 12 24 48; do
        idle_check
        bash -c "$(pin $c) $PY $R/concurrency.py $index $2 --system $5 --mode $6 --callers $c --queries $3 --warmup $4" \
            2>> logs/s6/$5-$6.err | sed "s/}\$/, \"pin\": \"$(pin $c)\"}/" | tee -a $OUT
    done
}
date
for s in nind nind-8s lucene tantivy; do
    for m in threads processes; do run work/v1/msmarco-stop data/msmarco-stop/queries.tsv 4000 1000 $s $m; done
done
date
for s in nind nind-8s lucene tantivy; do
    for m in threads processes; do run work/v2/v2-stop data/v2-stop/queries.tsv 2000 300 $s $m; done
done
date; echo STAGE6 DONE
