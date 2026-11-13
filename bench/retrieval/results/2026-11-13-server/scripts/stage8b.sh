#!/usr/bin/env bash
# Stage 8b: what stage 8 couldn't build with 4.2.2: nind's one-index builds of MS MARCO v2.1 documents
# from 4M on (lexicons of over 16.7M words: "value over 3 bytes", fixed by NindPadFile's cap), their
# searches, the TREC topics on them, the 32-segment update; the same commands and settings.
source "$(dirname "$0")/common.sh"
W=work/v21doc
date; idle_check
drive --work $W --data data/v21doc-stop-4M --systems nind --phases build,search --warmup 4552
drive --work $W --data data/v21doc-stop-4M --systems nind-4t,nind-8t --phases search --warmup 4552
rm -rf $W/v21doc-stop-4M/nind/index
drive --work $W --data data/v21doc-stop --systems nind --phases build,search,update --repeats 2 --warmup 4552
drive --work $W --data data/v21doc-stop --systems nind-32s --phases update
drive --work $W --data data/v21doc-stop --systems nind-4t,nind-8t --phases search --repeats 2 --warmup 4552
drive --work $W --data data/v21doc --systems nind --phases build,search --warmup 4552
drive --work $W --data data/v21doc --systems nind-8t --phases search --warmup 4552
date
for d in v21doc-stop v21doc; do for work in $W work/v21doc-anserini; do
    mkdir -p $work/$d-trec/nind; ln -sfn $PWD/$W/$d/nind/index $work/$d-trec/nind/index
done; done
for d in v21doc-stop-trec v21doc-trec; do
    drive --work $W --data data/$d --systems nind --phases search --repeats 3 --warmup 215
    drive --work work/v21doc-anserini --data data/$d --systems nind --phases search --repeats 3 --warmup 215 --k1 0.9 --b 0.4
done
$PY $R/driver.py --work $W --report-only > /dev/null
$PY $R/driver.py --work work/v21doc-anserini --report-only > /dev/null
for d in v21doc-stop v21doc; do
    echo "verify $d nind"; $SOCKET0 $PY scripts/verify.py $W/$d/nind/index > logs/verify8-$d-nind.jsonl 2>&1
    tail -1 logs/verify8-$d-nind.jsonl | cut -c1-300
done
date; echo STAGE8B DONE
