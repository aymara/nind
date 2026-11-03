#!/usr/bin/env bash
# Bulk-build indexing throughput benchmark: converts an MS MARCO subset then
# builds the index with index_bulk.py's parallel write-once pipeline
# (see that script's docstring for why this is the fair way to measure
# nind's achievable indexing throughput, unlike run_indexing_benchmark.sh).
#
# usage: run_bulk_indexing_benchmark.sh <collection.tsv> <size>[,<size>...]
#
# example: ./bench/run_bulk_indexing_benchmark.sh /data/msmarco/collection.tsv 10000,100000,1000000,8841823

set -o errexit
set -o pipefail
set -o nounset

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

COLLECTION_TSV="${1:?usage: $0 <collection.tsv> <size>[,<size>...]}"
SIZES_CSV="${2:?usage: $0 <collection.tsv> <size>[,<size>...]}"

RESULTS_DIR="$REPO_ROOT/bench/results"
mkdir -p "$RESULTS_DIR"

IFS=',' read -ra SIZES <<< "$SIZES_CSV"
for size in "${SIZES[@]}"; do
    echo "=== size=$size ===" >&2
    run_dir="$RESULTS_DIR/msmarco_${size}"
    mkdir -p "$run_dir"
    docs_file="$run_dir/docs.txt"

    conversion_log="$run_dir/convert.log"
    if [[ ! -s "$docs_file" || ! -s "$conversion_log" ]]; then
        # convert.log is also what run_indexing_benchmark.sh reads its corpus stats from
        python3 "$SCRIPT_DIR/msmarco/convert_to_nind.py" "$COLLECTION_TSV" "$docs_file" --limit "$size" 2> "$conversion_log"
        sed 's/^/  convert: /' "$conversion_log" >&2
    fi

    rm -f "$run_dir"/docs_bulk.nind*
    (cd "$REPO_ROOT" && uv run python3 "$SCRIPT_DIR/index_bulk.py" "--workers" "24" "$docs_file" "$run_dir/docs_bulk" "$size")
done

echo "Results in $RESULTS_DIR/indexing_bulk.csv" >&2
