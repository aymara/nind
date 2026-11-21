#!/usr/bin/env bash
# capped.sh CAP LOG COMMAND...: COMMAND in a user cgroup of CAP bytes (page cache included, no swap);
# LOG gets the cgroup's peak, peak anonymous memory (sampled every 0.5 s) and its memory events.
cap=$1 log=$2; shift 2
exec systemd-run --user --scope -q -p MemoryMax=$cap -p MemorySwapMax=0 bash -c '
    cg=/sys/fs/cgroup$(cut -d: -f3 /proc/self/cgroup); log=$1; shift
    ( peak=0; while :; do a=$(awk "/^anon /{print \$2}" $cg/memory.stat); (( a > peak )) && peak=$a && echo $peak > $log.anon; sleep 0.5; done ) &
    sampler=$!
    "$@"; status=$?
    kill $sampler
    { echo "{\"cap\": \"$(cat $cg/memory.max)\", \"peak\": $(cat $cg/memory.peak), \"anon_peak\": $(cat $log.anon 2>/dev/null || echo 0), \"status\": $status, \"events\": \"$(tr "\n" " " < $cg/memory.events)\"}"; } > $log
    rm -f $log.anon; exit $status' capped "$log" "$@"
