#!/usr/bin/env bash
# Stage 10: nind with a3f44c6 (format v3: a smaller term index), after stage 9 (3a37604: the directory cache, compact builds, merges limited by
# bytes, the touched documents filtered), everything the benchmarks page shows of it again: stage 7's
# runs (MS MARCO v1 and BEIR; v2 passages 1M to 138M, stopwords removed and kept; an index larger
# than memory) and stage 8's (MS MARCO v2.1 documents); the same commands and settings. Lucene's,
# tantivy's and bm25s's results stay (the driver's last result wins).
source "$(dirname "$0")/common.sh"
SEARCH_ONLY=nind-4t,nind-8t,nind-8s-8t,nind-32s-8t
date; idle_check
# stage 1: MS MARCO v1, BEIR, builds and updates again
W=work/v1
for d in msmarco-stop msmarco; do
    drive --work $W --data data/$d --systems nind,nind+local,nind-8s,nind-32s --repeats 3
    drive --work $W --data data/$d --systems $SEARCH_ONLY --phases search --repeats 3
    date
done
for d in msmarco-stop-1M msmarco-stop-2M msmarco-stop-4M; do
    drive --work $W --data data/$d --systems nind,nind-8s,nind-32s --phases build,search --repeats 3
    drive --work $W --data data/$d --systems $SEARCH_ONLY --phases search --repeats 3
done
for d in scifact-stop nfcorpus-stop fiqa-stop; do
    drive --work $W --data data/$d --systems nind --phases build,search --repeats 3
done
for rep in 2 3; do
    drive --work work/v1-rep$rep --data data/msmarco-stop --systems nind,nind-8s,nind-32s --phases build,update
    rm -rf work/v1-rep$rep/msmarco-stop/nind*/index
done
$PY $R/driver.py --work $W --report-only > /dev/null
date; echo STAGE10 V1 DONE
# stage 2: MS MARCO v2, 1M to 138M
W=work/v2
for d in v2-stop-1M v2-stop-4M v2-stop-8M v2-stop-16M v2-stop-32M v2-stop-64M v2-stop; do
    case $d in v2-stop-16M|v2-stop) phases=build,search,update ;; *) phases=build,search ;; esac
    case $d in v2-stop) repeats=2 ;; *) repeats=1 ;; esac
    drive --work $W --data data/$d --systems nind,nind-8s,nind-32s --phases $phases --repeats $repeats --warmup 3903
    drive --work $W --data data/$d --systems $SEARCH_ONLY --phases search --repeats $repeats --warmup 3903
    date
    case $d in v2-stop) ;; *) rm -rf $W/$d/nind*/index ;; esac
done
for d in v2-16M v2; do
    drive --work $W --data data/$d --systems nind,nind-8s --phases build,search --warmup 3903
    drive --work $W --data data/$d --systems nind-8t,nind-8s-8t --phases search --warmup 3903
    date
done
$PY $R/driver.py --work $W --report-only > /dev/null
for d in v2-stop v2; do for s in nind nind-8s; do
    echo "verify $d $s"; $SOCKET0 $PY scripts/verify.py $W/$d/$s/index > logs/verify10-$d-$s.jsonl 2>&1; tail -1 logs/verify10-$d-$s.jsonl | cut -c1-300
done; done
date; echo STAGE10 V2 DONE
# MS MARCO v2.1 documents (stage 8's runs, nind only)
W=work/v21doc
for d in v21doc-stop-1M v21doc-stop-4M v21doc-stop; do
    case $d in v21doc-stop) phases=build,search,update; repeats=2 ;; *) phases=build,search; repeats=1 ;; esac
    drive --work $W --data data/$d --systems nind,nind-8s,nind-32s --phases $phases --repeats $repeats --warmup 4552
    drive --work $W --data data/$d --systems $SEARCH_ONLY --phases search --repeats $repeats --warmup 4552
    date
    case $d in v21doc-stop) ;; *) rm -rf $W/$d/nind*/index $W/$d/nind*/index-update ;; esac
done
drive --work $W --data data/v21doc --systems nind,nind-8s --phases build,search --warmup 4552
drive --work $W --data data/v21doc --systems nind-8t,nind-8s-8t --phases search --warmup 4552
for d in v21doc-stop-trec v21doc-trec; do
    case $d in v21doc-stop-trec) s=nind,nind-8s,nind-32s ;; *) s=nind,nind-8s ;; esac
    drive --work $W --data data/$d --systems $s --phases search --repeats 3 --warmup 215
    drive --work work/v21doc-anserini --data data/$d --systems $s --phases search --repeats 3 --warmup 215 --k1 0.9 --b 0.4
done
$PY $R/driver.py --work $W --report-only > /dev/null
$PY $R/driver.py --work work/v21doc-anserini --report-only > /dev/null
for d in v21doc-stop v21doc; do for s in nind nind-8s; do
    echo "verify $d $s"; $SOCKET0 $PY scripts/verify.py $W/$d/$s/index > logs/verify10-$d-$s.jsonl 2>&1
    tail -1 logs/verify10-$d-$s.jsonl | cut -c1-300
done; done
date; echo STAGE10 V21DOC DONE
# stage 5: an index larger than memory, nind (caps from an uncapped round of this version)
SYSTEMS="nind nind-8s-8t"
index_of() { case $1 in nind-8s-8t) echo nind-8s ;; *) echo $1 ;; esac; }
link() {
    local i=$(index_of $2); mkdir -p $1/v2-stop/$i; ln -sfn $PWD/work/v2/v2-stop/$i/index $1/v2-stop/$i/index
    mkdir -p $1/v2-stop/$2
}
run() {
    $PY scripts/evict.py work/v2/v2-stop/$(index_of $2)/index data/v2-stop
    scripts/capped.sh $3 $4 timeout 7200 $SOCKET0 $PY $R/driver.py --work $1 --data data/v2-stop --systems $2 \
        --phases search --max-queries $5 --warmup $6 2>&1 | grep -E ' \.\.\. '
    echo "$2 cap $3 $(cat $4)"
}
mkdir -p logs/s10
declare -A anon
for s in $SYSTEMS; do
    link work/s10-infinity $s
    run work/s10-infinity $s infinity logs/s10/infinity-$s.json 2000 500
    anon[$s]=$($PY -c "import json; print(json.load(open('logs/s10/infinity-$s.json'))['anon_peak'])")
done
for budget in 8 4 2 1; do
    for s in $SYSTEMS; do
        link work/s10-$budget $s
        run work/s10-$budget $s $(( anon[$s] * 11 / 10 + budget * 2**30 )) logs/s10/$budget-$s.json 2000 500
    done
    date
done
date; echo STAGE10 DONE
