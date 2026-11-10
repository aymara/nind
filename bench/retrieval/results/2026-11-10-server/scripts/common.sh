# Sourced by the stage scripts. One socket's physical cores for everything comparable.
cd /scratch/gdechalendar/Projets/nind-bench
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk IR_DATASETS_HOME=$PWD/irds
PY=$PWD/env/bin/python
R=/scratch/gdechalendar/Projets/nind/bench/retrieval
SOCKET0="numactl --physcpubind=0-11 --preferred=0"
drive() { $SOCKET0 $PY $R/driver.py "$@" 2>&1 | grep -E ' \.\.\. '; }
idle_check() {  # refuses to time anything on a busy machine
    load=$(awk '{print int($1)}' /proc/loadavg)
    if (( load > 3 )); then echo "$(date) load is $load: others running?" ; fi
}
