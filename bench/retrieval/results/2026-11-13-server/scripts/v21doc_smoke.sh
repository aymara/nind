#!/usr/bin/env bash
# Every system on MS MARCO v2.1 documents' first 1M, 200 queries: does anything break on long documents?
source "$(dirname "$0")/common.sh"
W=work/v21doc-smoke
$SOCKET0 $PY $R/driver.py --work $W --data data/v21doc-stop-1M --systems nind,nind-8s,lucene,tantivy \
    --phases build,search,update --max-queries 200 --warmup 50
$SOCKET0 $PY $R/driver.py --work $W --data data/v21doc-stop-1M --systems nind-8t,nind-8s-8t \
    --phases search --max-queries 200 --warmup 50
date; echo SMOKE DONE
