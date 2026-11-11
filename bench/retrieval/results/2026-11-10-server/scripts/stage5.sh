#!/usr/bin/env bash
# Stage 5: an index larger than memory. MS MARCO v2 138M (stopwords removed): each system in a user
# cgroup capped at its own peak anonymous memory (+10%) plus a page-cache budget; the index evicted
# first, so every page it reads is charged to the cgroup. Budgets from all of it to 1 GB (indexes:
# 11-15 GB). The first 2000 queries after a warm-up of 500; one pass.
source "$(dirname "$0")/common.sh"
SYSTEMS="nind nind-8s-8t lucene tantivy"
index_of() { case $1 in nind-8s-8t) echo nind-8s ;; *) echo $1 ;; esac; }
link() {  # link() WORK SYSTEM: WORK/v2-stop/<index>/index -> stage 2's
    local i=$(index_of $2); mkdir -p $1/v2-stop/$i; ln -sfn $PWD/work/v2/v2-stop/$i/index $1/v2-stop/$i/index
    mkdir -p $1/v2-stop/$2
}
run() {  # run() WORK SYSTEM CAP LOG QUERIES WARMUP
    $PY scripts/evict.py work/v2/v2-stop/$(index_of $2)/index data/v2-stop
    scripts/capped.sh $3 $4 timeout 7200 $SOCKET0 $PY $R/driver.py --work $1 --data data/v2-stop --systems $2 \
        --phases search --max-queries $5 --warmup $6 2>&1 | grep -E ' \.\.\. '
    echo "$2 $(cat $4)"
}
date; idle_check
mkdir -p logs/s5
declare -A anon
for s in $SYSTEMS; do   # own memory: a short uncapped run
    link work/s5-calib $s
    run work/s5-calib $s infinity logs/s5/calib-$s.json 200 50
    anon[$s]=$($PY -c "import json; print(json.load(open('logs/s5/calib-$s.json'))['anon_peak'])")
done
for budget in infinity 8 4 2 1; do
    for s in $SYSTEMS; do
        link work/s5-$budget $s
        if [[ $budget == infinity ]]; then cap=infinity; else cap=$(( anon[$s] * 11 / 10 + budget * 2**30 )); fi
        run work/s5-$budget $s $cap logs/s5/$budget-$s.json 2000 500
    done
    date
done
date; echo STAGE5 DONE
