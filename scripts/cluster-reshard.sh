#!/usr/bin/env bash
# Moves slots between two masters of the local cluster — the M4 acceptance
# knob: run this while a load test drives the proxy and check the client sees
# zero errors (docs/development-plan.md M4).
#
#   ./scripts/cluster-reshard.sh [slots] [--from PORT] [--to PORT] [--rounds N]
#
# Defaults: 1000 slots, from the first master to the second, one round. With
# --rounds N the transfer alternates direction each round, so a long load test
# keeps hitting MOVED/ASK instead of settling after the first move.
set -euo pipefail

SLOTS="${1:-1000}"
if [[ "${SLOTS}" == --* ]]; then
  SLOTS=1000
else
  shift || true
fi
BASE_PORT=7000
FROM_PORT=""
TO_PORT=""
ROUNDS=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --from) FROM_PORT="$2"; shift 2 ;;
    --to) TO_PORT="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    --base-port) BASE_PORT="$2"; shift 2 ;;
    -h|--help) sed -n '2,11p' "$0" | sed 's|^# \?||'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

CLI="$(command -v valkey-cli || command -v redis-cli || true)"
if [[ -z "${CLI}" ]]; then
  echo "need valkey-cli or redis-cli on PATH" >&2
  exit 1
fi

FROM_PORT="${FROM_PORT:-${BASE_PORT}}"
TO_PORT="${TO_PORT:-$((BASE_PORT + 1))}"

# --cluster reshard wants node IDs, not addresses.
node_id() {
  "${CLI}" -p "$1" cluster myid
}

# redis-cli --cluster exits 0 even when it prints "[ERR] Nodes don't agree about
# configuration!" or refuses to reshard, so every check here greps the output.
#
# A reshard bumps config epochs, and the next round starts before they have
# propagated: without this wait, round 2 aborts with "Please fix your cluster
# problems before resharding" and (exit code being useless) the script would
# sail on reporting success.
wait_agreement() {
  for ((i = 0; i < 100; i++)); do
    if "${CLI}" --cluster check "127.0.0.1:${BASE_PORT}" 2>&1 | grep -q 'All nodes agree'; then
      return 0
    fi
    sleep 0.2
  done
  echo "cluster never converged; run '${CLI} --cluster check 127.0.0.1:${BASE_PORT}'" >&2
  return 1
}

for ((round = 1; round <= ROUNDS; round++)); do
  if (( round % 2 == 1 )); then
    src="${FROM_PORT}"; dst="${TO_PORT}"
  else
    src="${TO_PORT}"; dst="${FROM_PORT}"
  fi
  wait_agreement
  echo "round ${round}/${ROUNDS}: ${SLOTS} slots ${src} -> ${dst}"
  out="$("${CLI}" --cluster reshard "127.0.0.1:${BASE_PORT}" \
    --cluster-from "$(node_id "${src}")" \
    --cluster-to "$(node_id "${dst}")" \
    --cluster-slots "${SLOTS}" \
    --cluster-yes 2>&1)" || true
  if grep -qE '^\*\*\*|\[ERR\]' <<<"${out}"; then
    grep -vE '^Moving slot|^ *Moving' <<<"${out}" | tail -20 >&2
    echo "round ${round} failed" >&2
    exit 1
  fi
  echo "  moved ${SLOTS} slot(s)"
done

wait_agreement
"${CLI}" --cluster check "127.0.0.1:${BASE_PORT}" | tail -5
