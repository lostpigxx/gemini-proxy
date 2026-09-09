#!/usr/bin/env bash
# Brings up a local 3-master / 3-replica valkey cluster on 127.0.0.1:7000-7005.
#
#   ./scripts/cluster-up.sh [--masters N] [--replicas N] [--base-port P]
#
# Six ordinary server processes in one place rather than docker compose: the
# proxy, the load generator and the resharding commands all run in the same
# Linux box (or verification container) anyway, and compose would add image
# pulls plus cluster announce-ip plumbing for nothing. Rationale in
# docs/design/m4-cluster-routing.md §8.
#
# State (configs, logs, pidfiles, RDBs) lives under $VKP_CLUSTER_DIR, default
# /tmp/vkp-cluster. Tear down with scripts/cluster-down.sh.
set -euo pipefail

MASTERS=3
REPLICAS=1
BASE_PORT=7000
CLUSTER_DIR="${VKP_CLUSTER_DIR:-/tmp/vkp-cluster}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --masters) MASTERS="$2"; shift 2 ;;
    --replicas) REPLICAS="$2"; shift 2 ;;
    --base-port) BASE_PORT="$2"; shift 2 ;;
    -h|--help) sed -n '2,13p' "$0" | sed 's|^# \?||'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

# valkey first, redis as the fallback: the verification container has redis
# 7.0.15, which speaks everything the proxy needs (CLUSTER SHARDS included).
SERVER="$(command -v valkey-server || command -v redis-server || true)"
CLI="$(command -v valkey-cli || command -v redis-cli || true)"
if [[ -z "${SERVER}" || -z "${CLI}" ]]; then
  echo "need valkey-server/valkey-cli (or the redis equivalents) on PATH" >&2
  exit 1
fi

NODES=$((MASTERS + MASTERS * REPLICAS))
LAST_PORT=$((BASE_PORT + NODES - 1))

# Refuse to stomp on an existing cluster rather than half-joining it.
for ((port = BASE_PORT; port <= LAST_PORT; port++)); do
  if "${CLI}" -p "${port}" ping >/dev/null 2>&1; then
    echo "port ${port} already answers PING; run scripts/cluster-down.sh first" >&2
    exit 1
  fi
done

rm -rf "${CLUSTER_DIR}"
mkdir -p "${CLUSTER_DIR}"

for ((port = BASE_PORT; port <= LAST_PORT; port++)); do
  node_dir="${CLUSTER_DIR}/${port}"
  mkdir -p "${node_dir}"
  cat > "${node_dir}/node.conf" <<EOF
port ${port}
bind 127.0.0.1
cluster-enabled yes
cluster-config-file nodes.conf
cluster-node-timeout 5000
appendonly no
save ""
dir ${node_dir}
logfile ${node_dir}/server.log
pidfile ${node_dir}/server.pid
daemonize no
EOF
  "${SERVER}" "${node_dir}/node.conf" &
  echo $! > "${node_dir}/spawn.pid"
done

echo "waiting for ${NODES} nodes on ${BASE_PORT}-${LAST_PORT} ..."
for ((port = BASE_PORT; port <= LAST_PORT; port++)); do
  for ((i = 0; i < 100; i++)); do
    if [[ "$("${CLI}" -p "${port}" ping 2>/dev/null || true)" == "PONG" ]]; then
      break
    fi
    sleep 0.1
  done
  if [[ "$("${CLI}" -p "${port}" ping 2>/dev/null || true)" != "PONG" ]]; then
    echo "node ${port} never came up; see ${CLUSTER_DIR}/${port}/server.log" >&2
    exit 1
  fi
done

ADDRS=()
for ((port = BASE_PORT; port <= LAST_PORT; port++)); do
  ADDRS+=("127.0.0.1:${port}")
done
"${CLI}" --cluster create "${ADDRS[@]}" --cluster-replicas "${REPLICAS}" --cluster-yes

# --cluster create returns before every node agrees; the proxy would just get
# CLUSTERDOWN, so block until the slot map has actually converged.
echo "waiting for cluster_state:ok ..."
for ((i = 0; i < 200; i++)); do
  ok=0
  for ((port = BASE_PORT; port <= LAST_PORT; port++)); do
    if "${CLI}" -p "${port}" cluster info 2>/dev/null | grep -q '^cluster_state:ok'; then
      ok=$((ok + 1))
    fi
  done
  if [[ "${ok}" -eq "${NODES}" ]]; then
    break
  fi
  sleep 0.1
done
if [[ "${ok}" -ne "${NODES}" ]]; then
  echo "only ${ok}/${NODES} nodes report cluster_state:ok" >&2
  exit 1
fi

SEEDS="$(IFS=,; echo "${ADDRS[*]:0:${MASTERS}}")"
echo
echo "cluster up: ${MASTERS} master(s), ${REPLICAS} replica(s) each, state dir ${CLUSTER_DIR}"
echo "  proxyd --cluster-seeds ${SEEDS}"
echo "  ./scripts/cluster-reshard.sh 1000   # move slots while a load test runs"
echo "  ./scripts/cluster-down.sh"
