#!/bin/bash
# Start (or stop) a Milvus standalone server in Docker, with embedded etcd and local
# storage, as Milvus's scripts/standalone_embed.sh does.
#
#   examples/milvus/milvus-standalone.sh start [config dir]   # default ./milvus-config
#   examples/milvus/milvus-standalone.sh stop                  # the data stays (volume $NAME-data)
#   examples/milvus/milvus-standalone.sh purge                 # stop and remove the data
#
# MILVUS_IMAGE (default milvusdb/milvus:v3.0.6), MILVUS_PORT (default 19530),
# MILVUS_NAME (container name, default nind-milvus).
set -euo pipefail

IMAGE=${MILVUS_IMAGE:-milvusdb/milvus:v3.0.6}
PORT=${MILVUS_PORT:-19530}
NAME=${MILVUS_NAME:-nind-milvus}

case "${1:-}" in
start)
    # (the image runs as uid 999: its data in a named volume, not a host directory)
    DATA=$(realpath -m "${2:-./milvus-config}")
    mkdir -p "$DATA"
    cat > "$DATA/embedEtcd.yaml" <<EOF
listen-client-urls: http://0.0.0.0:2379
advertise-client-urls: http://0.0.0.0:2379
quota-backend-bytes: 4294967296
auto-compaction-mode: revision
auto-compaction-retention: '1000'
EOF
    touch "$DATA/user.yaml"
    docker run -d --name "$NAME" --security-opt seccomp:unconfined \
        -e ETCD_USE_EMBED=true -e ETCD_DATA_DIR=/var/lib/milvus/etcd \
        -e ETCD_CONFIG_PATH=/milvus/configs/embedEtcd.yaml \
        -e COMMON_STORAGETYPE=local -e DEPLOY_MODE=STANDALONE \
        -v "$NAME-data:/var/lib/milvus" \
        -v "$DATA/embedEtcd.yaml:/milvus/configs/embedEtcd.yaml:ro" \
        -v "$DATA/user.yaml:/milvus/configs/user.yaml:ro" \
        -p "127.0.0.1:$PORT:19530" \
        --health-cmd="curl -f http://localhost:9091/healthz" --health-interval=10s \
        --health-start-period=60s --health-timeout=10s --health-retries=3 \
        "$IMAGE" milvus run standalone > /dev/null
    printf "waiting for Milvus (%s) on port %s" "$IMAGE" "$PORT"
    until [ "$(docker inspect -f '{{.State.Health.Status}}' "$NAME")" = healthy ]; do
        if [ "$(docker inspect -f '{{.State.Running}}' "$NAME")" != true ]; then
            echo; docker logs --tail 30 "$NAME"; exit 1
        fi
        printf .; sleep 2
    done
    echo " ready: http://127.0.0.1:$PORT"
    ;;
stop)
    docker rm -f "$NAME" > /dev/null && echo "stopped $NAME"
    ;;
purge)
    docker rm -f "$NAME" > /dev/null 2>&1 || true
    docker volume rm "$NAME-data" > /dev/null && echo "removed $NAME and its data"
    ;;
*)
    echo "usage: $0 start [config dir] | stop | purge" >&2; exit 2
    ;;
esac
