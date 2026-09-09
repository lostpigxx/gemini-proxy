#!/usr/bin/env bash
# Tears down the cluster started by scripts/cluster-up.sh.
#
#   ./scripts/cluster-down.sh [--keep-dir]
#
# Safe to run when nothing is up. Honours $VKP_CLUSTER_DIR the same way.
set -euo pipefail

CLUSTER_DIR="${VKP_CLUSTER_DIR:-/tmp/vkp-cluster}"
KEEP_DIR=0
if [[ "${1:-}" == "--keep-dir" ]]; then
  KEEP_DIR=1
fi

if [[ ! -d "${CLUSTER_DIR}" ]]; then
  echo "nothing to do: ${CLUSTER_DIR} does not exist"
  exit 0
fi

# A process we killed can linger as a zombie: its spawner shell is long gone
# and a container's PID 1 does not necessarily reap. kill -0 succeeds on a
# zombie, so without the state check the wait loop below would spin its full
# timeout and then kill -9 a corpse.
alive() {
  kill -0 "$1" 2>/dev/null || return 1
  [[ -r "/proc/$1/stat" ]] || return 0  # no procfs: trust kill -0
  # Everything up to the last ')' is pid + comm (which may itself contain
  # spaces and parens); the field right after it is the state. Z = reaped-ish.
  [[ "$(sed 's/.*) //' "/proc/$1/stat" | cut -d' ' -f1)" != "Z" ]]
}

# server.pid and spawn.pid normally name the same process; dedupe so the count
# reported at the end is nodes, not pidfiles.
pids=()
for pidfile in "${CLUSTER_DIR}"/*/server.pid "${CLUSTER_DIR}"/*/spawn.pid; do
  [[ -f "${pidfile}" ]] || continue
  pid="$(cat "${pidfile}")"
  [[ -n "${pid}" ]] && pids+=("${pid}")
done
if [[ "${#pids[@]}" -gt 0 ]]; then
  mapfile -t pids < <(printf '%s\n' "${pids[@]}" | sort -un)
fi

killed=0
for pid in ${pids[@]+"${pids[@]}"}; do
  if alive "${pid}"; then
    kill "${pid}" 2>/dev/null || true
    killed=$((killed + 1))
  fi
done

# Give them a moment to exit on SIGTERM before insisting.
for ((i = 0; i < 50; i++)); do
  remaining=0
  for pid in ${pids[@]+"${pids[@]}"}; do
    alive "${pid}" && remaining=$((remaining + 1))
  done
  [[ "${remaining}" -eq 0 ]] && break
  sleep 0.1
done
for pid in ${pids[@]+"${pids[@]}"}; do
  alive "${pid}" && kill -9 "${pid}" 2>/dev/null || true
done

if [[ "${KEEP_DIR}" -eq 0 ]]; then
  rm -rf "${CLUSTER_DIR}"
  echo "stopped ${killed} process(es), removed ${CLUSTER_DIR}"
else
  echo "stopped ${killed} process(es), kept ${CLUSTER_DIR}"
fi
