#!/usr/bin/env bash
# Stage 3: what the cores give. Up to 12 threads: socket 0's physical cores; 24: both sockets' physical
# cores; 48: every hardware thread.
source "$(dirname "$0")/common.sh"
pin() { if (( $1 <= 12 )); then echo "numactl --physcpubind=0-11 --preferred=0"; elif (( $1 <= 24 )); then echo "numactl --physcpubind=0-23"; else echo ""; fi; }
date; idle_check
# a) threads per query (one query at a time)
for spec in v1/msmarco-stop v1/msmarco v2/v2-stop v2/v2; do
    data=data/${spec#*/}
    for s in nind nind-8s nind-32s; do
        index=work/$spec/$s/index
        [[ -d $index ]] || continue
        for t in 1 2 4 8 12 16 24; do
            $(pin $t) $PY scripts/search_threads.py $index $data/queries.tsv $t >> logs/stage3-search-threads.jsonl
        done
    done
    date
done
# b) queries at once: one engine shared by T threads (one thread per query)
for spec in v1/msmarco-stop v2/v2-stop v2/v2; do
    data=data/${spec#*/}
    for s in nind nind-8s nind-32s; do
        index=work/$spec/$s/index
        [[ -d $index ]] || continue
        for t in 1 2 4 8 12 24 48; do
            echo "{\"index\": \"$index\", \"pin\": \"$(pin $t)\"}" >> logs/stage3-concurrency.jsonl
            $(pin $t) $PY $R/concurrency.py $index $data/queries.tsv --threads $t --queries 100000 >> logs/stage3-concurrency.jsonl
        done
    done
    date
done
# c) build threads, 8.8M, then 48 threads at 138M
for t in 1 2 4 8 12 24 48; do
    $(pin $t) $PY $R/driver.py --work work/s3-build-$t --data data/msmarco-stop --systems nind,nind-32s,lucene,tantivy --phases build --threads $t 2>&1 | grep -E ' \.\.\. '
    rm -rf work/s3-build-$t/msmarco-stop/*/index
done
date
$PY $R/driver.py --work work/s3-build-138M-48 --data data/v2-stop --systems nind-32s,lucene,tantivy --phases build --threads 48 2>&1 | grep -E ' \.\.\. '
rm -rf work/s3-build-138M-48/v2-stop/*/index
date; echo STAGE3 DONE
