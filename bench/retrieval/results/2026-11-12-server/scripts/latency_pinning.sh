#!/usr/bin/env bash
# Ten processes pinned to one CPU against ten free on socket 0's 12 physical cores, alternated:
# MS MARCO v1 8.8M, stopwords removed, top 10, one thread (docs/TODO.md, "Latency varies between processes").
cd /scratch/gdechalendar/Projets/nind-bench
PY=/scratch/gdechalendar/Projets/nind/.venv/bin/python
I=work/v1/msmarco-stop/nind/index Q=data/msmarco-stop/queries.tsv
for run in $(seq 1 10); do
    numactl --physcpubind=5 --membind=0 $PY scripts/latency_pinning.py $I $Q cpu1
    numactl --physcpubind=0-11 --membind=0 $PY scripts/latency_pinning.py $I $Q cpu12
done
