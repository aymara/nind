#!/usr/bin/env bash
# Indexing throughput & index size benchmark, driven off the MS MARCO Passage
# Ranking collection.tsv, using nind's real bulk indexer (Nind_indexeCorpus)
# rather than the one-file-per-document nind_engine.NindIndexer.
#
# usage: run_indexing_benchmark.sh <collection.tsv> <size>[,<size>...] [<bin-dir>]
#   <bin-dir> defaults to $NIND_DIST/bin
#
# example: NIND_DIST=/path/to/dist ./bench/run_indexing_benchmark.sh \
#             /data/msmarco/collection.tsv 10000,100000,1000000

set -o errexit
set -o pipefail
set -o nounset

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

COLLECTION_TSV="${1:?usage: $0 <collection.tsv> <size>[,<size>...] [<bin-dir>]}"
SIZES_CSV="${2:?usage: $0 <collection.tsv> <size>[,<size>...] [<bin-dir>]}"
BIN_DIR="${3:-${NIND_DIST:-}/bin}"

INDEXEUR="$BIN_DIR/Nind_indexeCorpus"
if [[ ! -x "$INDEXEUR" ]]; then
    echo "Nind_indexeCorpus not found/executable at: $INDEXEUR" >&2
    echo "Build it first: NIND_DIST=<prefix> ./gbuild.sh -m Release" >&2
    exit 1
fi
# gbuild.sh installs with an empty RPATH (see CMakeLists.txt), so the
# sibling lib/ dir must be on LD_LIBRARY_PATH for these binaries to run.
export LD_LIBRARY_PATH="$(cd "$BIN_DIR/../lib" && pwd)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

RESULTS_DIR="$REPO_ROOT/bench/results"
mkdir -p "$RESULTS_DIR"
CSV="$RESULTS_DIR/indexing.csv"
if [[ ! -f "$CSV" ]]; then
    echo "size,docs,tokens,vocab,input_bytes,wall_seconds,docs_per_sec,mb_per_sec,peak_rss_kb,lexicon_bytes,term_bytes,local_bytes,total_index_bytes" > "$CSV"
fi

nearest_prime_above() {
    python3 "$REPO_ROOT/src/py/nind/Nind_trouveNombresPremiers.py" "$1" | head -1
}

IFS=',' read -ra SIZES <<< "$SIZES_CSV"
for size in "${SIZES[@]}"; do
    echo "=== size=$size ===" >&2
    run_dir="$RESULTS_DIR/msmarco_${size}"
    mkdir -p "$run_dir"
    docs_file="$run_dir/docs.txt"
    conversion_log="$run_dir/convert.log"

    # the run dir is shared with run_bulk_indexing_benchmark.sh: only remove
    # this benchmark's own index files, and reuse an existing conversion
    rm -f "$run_dir"/docs.nind*
    if [[ ! -s "$docs_file" || ! -s "$conversion_log" ]]; then
        python3 "$SCRIPT_DIR/msmarco/convert_to_nind.py" "$COLLECTION_TSV" "$docs_file" --limit "$size" 2> "$conversion_log"
    fi
    stats_line="$(cat "$conversion_log")"
    docs=$(grep -oP 'docs=\K[0-9]+' <<< "$stats_line")
    tokens=$(grep -oP 'tokens=\K[0-9]+' <<< "$stats_line")
    vocab=$(grep -oP 'vocab=\K[0-9]+' <<< "$stats_line")
    input_bytes=$(stat -c '%s' "$docs_file")

    lexicon_size=$(nearest_prime_above $(( vocab * 12 / 10 + 10 )))
    inverse_size=$(nearest_prime_above $(( vocab * 12 / 10 + 10 )))
    locaux_size=$(nearest_prime_above $(( docs + 10 )))

    time_log="$run_dir/time.log"
    index_log="$run_dir/index.log"
    /usr/bin/time -v "$INDEXEUR" "$docs_file" "$lexicon_size" "$inverse_size" "$locaux_size" \
        > "$index_log" 2> "$time_log"

    wall_raw=$(grep -oP 'Elapsed \(wall clock\) time.*: \K.*' "$time_log")
    wall_seconds=$(awk -F: '{
        if (NF == 3) print $1*3600 + $2*60 + $3;
        else if (NF == 2) print $1*60 + $2;
        else print $1;
    }' <<< "$wall_raw")
    peak_rss_kb=$(grep -oP 'Maximum resident set size \(kbytes\): \K[0-9]+' "$time_log")

    lexicon_bytes=$(stat -c '%s' "$run_dir/docs.nindlexiconindex" 2>/dev/null || echo 0)
    term_bytes=$(stat -c '%s' "$run_dir/docs.nindtermindex" 2>/dev/null || echo 0)
    local_bytes=$(stat -c '%s' "$run_dir/docs.nindlocalindex" 2>/dev/null || echo 0)
    total_index_bytes=$(( lexicon_bytes + term_bytes + local_bytes ))

    docs_per_sec=$(awk -v d="$docs" -v s="$wall_seconds" 'BEGIN { print (s > 0) ? d/s : d }')
    mb_per_sec=$(awk -v b="$input_bytes" -v s="$wall_seconds" 'BEGIN { print (s > 0) ? (b/1048576)/s : b/1048576 }')

    echo "$size,$docs,$tokens,$vocab,$input_bytes,$wall_seconds,$docs_per_sec,$mb_per_sec,$peak_rss_kb,$lexicon_bytes,$term_bytes,$local_bytes,$total_index_bytes" >> "$CSV"
    echo "docs=$docs wall=${wall_seconds}s docs/sec=$docs_per_sec MB/sec=$mb_per_sec peak_rss=${peak_rss_kb}KB total_index=${total_index_bytes}B" >&2
done

echo "Results appended to $CSV" >&2
