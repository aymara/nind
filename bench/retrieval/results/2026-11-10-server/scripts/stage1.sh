#!/usr/bin/env bash
# Stage 1: every system on MS MARCO v1 and BEIR, this machine's baseline; stage 4's statistics
# (every query, 3 passes per k, 3 builds).
source "$(dirname "$0")/common.sh"
W=work/v1
FULL=nind,nind+local,nind-8s,nind-32s,lucene,lucene+pos,tantivy
SEARCH_ONLY=nind-4t,nind-8t,nind-8s-8t,nind-32s-8t
date; idle_check
for d in msmarco-stop msmarco; do
    drive --work $W --data data/$d --systems $FULL --repeats 3
    drive --work $W --data data/$d --systems $SEARCH_ONLY --phases search --repeats 3
    # exhaustive at this size, ~0.4 s per query: the 500 queries of the earlier runs, one pass
    drive --work $W --data data/$d --systems bm25s --max-queries 500
    date
done
for d in msmarco-stop-1M msmarco-stop-2M msmarco-stop-4M; do
    drive --work $W --data data/$d --systems nind,nind-8s,nind-32s,lucene,tantivy --phases build,search --repeats 3
    drive --work $W --data data/$d --systems $SEARCH_ONLY --phases search --repeats 3
done
for d in scifact-stop nfcorpus-stop fiqa-stop; do
    drive --work $W --data data/$d --systems nind,lucene,tantivy,bm25s --phases build,search --repeats 3
done
date
# build and update again, for their spread
for rep in 2 3; do
    drive --work work/v1-rep$rep --data data/msmarco-stop --systems nind,nind-8s,nind-32s,lucene,tantivy --phases build,update
    rm -rf work/v1-rep$rep/msmarco-stop/*/index
done
$PY $R/driver.py --work $W --report-only > /dev/null
date; echo STAGE1 DONE
