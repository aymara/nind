#!/usr/bin/env bash
# Stage 2: MS MARCO v2, prefixes up to 138M passages. nind direct and in segments, Lucene, tantivy.
# bm25s left out: ~14 GB at 8.8M and exhaustive, it needs ~220 GB and many seconds per query at 138M.
# Stage 1 showed passes 2 and 3 within a few percent, the first one slower for threaded searches: a
# whole pass of warm-up, then one measured pass per k (two at 138M).
source "$(dirname "$0")/common.sh"
W=work/v2
SYSTEMS=nind,nind-8s,nind-32s,lucene,tantivy
SEARCH_ONLY=nind-4t,nind-8t,nind-8s-8t,nind-32s-8t
date; idle_check
for d in v2-stop-1M v2-stop-4M v2-stop-8M v2-stop-16M v2-stop-32M v2-stop-64M v2-stop; do
    case $d in v2-stop-16M|v2-stop) phases=build,search,update ;; *) phases=build,search ;; esac
    case $d in v2-stop) repeats=2 ;; *) repeats=1 ;; esac
    drive --work $W --data data/$d --systems $SYSTEMS --phases $phases --repeats $repeats --warmup 3903
    drive --work $W --data data/$d --systems $SEARCH_ONLY --phases search --repeats $repeats --warmup 3903
    date
    # only the largest indexes are kept for stage 3
    case $d in v2-stop) ;; *) rm -rf $W/$d/*/index ;; esac
done
# stopwords kept: very long postings lists (a term in most of 138M documents)
for d in v2-16M v2; do
    drive --work $W --data data/$d --systems nind,nind-8s,lucene,tantivy --phases build,search --warmup 3903
    drive --work $W --data data/$d --systems nind-8t,nind-8s-8t --phases search --warmup 3903
    date
done
$PY $R/driver.py --work $W --report-only > /dev/null
# the format at this size: every definition of every segment read back
for d in v2-stop v2; do for s in nind nind-8s; do
    echo "verify $d $s"; $SOCKET0 $PY scripts/verify.py $W/$d/$s/index > logs/verify-$d-$s.jsonl 2>&1; tail -1 logs/verify-$d-$s.jsonl | cut -c1-300
done; done
date; echo STAGE2 DONE
