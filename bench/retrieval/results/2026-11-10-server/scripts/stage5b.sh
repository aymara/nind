#!/usr/bin/env bash
# Stage 5, capped budgets: caps from the uncapped round's peak anonymous memory (the short
# calibration runs underestimated it: 6.97 vs 8.39 GB for nind-8s-8t). Waits for that round, then
# stops stage5.sh before its capped rounds.
source "$(dirname "$0")/common.sh"
while [[ ! -s logs/s5/infinity-tantivy.json ]]; do sleep 1; done
pkill -f '[s]cripts/stage5\.sh'; sleep 1
# a capped run it may have started meanwhile
pkill -f '[d]river.py --work work/s5-8 '; pkill -f '[w]orker.py'; sleep 5; rm -rf work/s5-8 logs/s5/8-*.json
SYSTEMS="nind nind-8s-8t lucene tantivy"
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
date
for budget in 8 4 2 1; do
    for s in $SYSTEMS; do
        anon=$($PY -c "import json; print(json.load(open('logs/s5/infinity-$s.json'))['anon_peak'])")
        link work/s5-$budget $s
        run work/s5-$budget $s $(( anon * 11 / 10 + budget * 2**30 )) logs/s5/$budget-$s.json 2000 500
    done
    date
done
date; echo STAGE5 DONE
