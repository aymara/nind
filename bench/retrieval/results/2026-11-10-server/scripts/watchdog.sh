#!/usr/bin/env bash
# Stops the benchmark rather than let it take the machine: kills the process group PGID when
# available memory drops under the threshold (default 15 GB), and leaves a trace in the log.
# usage: watchdog.sh PID [GB]
pid=$1; limit=$(( ${2:-15} * 1024 * 1024 ))
while kill -0 "$pid" 2>/dev/null; do
    avail=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
    if (( avail < limit )); then
        echo "$(date) WATCHDOG: MemAvailable ${avail} kB under ${limit} kB, killing tree of $pid" >&2
        pkill -TERM -P "$pid"; sleep 5; pkill -KILL -g "$(ps -o pgid= "$pid" | tr -d ' ')"
        exit 1
    fi
    sleep 2
done
