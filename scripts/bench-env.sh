#!/usr/bin/env bash
# Brings up a CPU-pinned benchmark environment: backend(s) on their own cores,
# one or more proxy binaries on theirs, benchmark threads on a third set.
#
#   ./scripts/bench-env.sh up                       # standalone backend :6399
#   ./scripts/bench-env.sh up --cluster             # + 3-master cluster :7000-7002
#   ./scripts/bench-env.sh proxy head /cache/m-cl-rel/src/proxyd
#   ./scripts/bench-env.sh proxy old  /cache/m6-base/src/proxyd
#   ./scripts/bench-env.sh arms                     # --arm flags for bench.sh
#   ./scripts/bench-env.sh down
#
# Why pin. This box's redis-benchmark numbers looked bimodal for three
# milestones running (`-P 16` direct read as either ~2.0M or ~3.3M rps) and the
# working theory was scheduler placement of the single-threaded server. It was:
# with the backend on a dedicated core and the benchmark threads on their own
# set, four consecutive runs return *byte-identical* rps and percentiles. An
# unpinned reference arm is noisier than every optimisation M6 is trying to
# measure, so pinning is not tuning here — it is the measurement instrument.
#
# Give the load generator room. redis-benchmark is the bottleneck long before
# the proxy is: on four cores every arm reads back the same 363k rps at `-P 1`
# and direct `-P 16` collapses from 2.86M to 1.43M — measuring the client, not
# the server. Eight cores for four benchmark threads is what made `-P 16`
# reproduce exactly.
#
# Every proxy arm shares one cpuset on purpose: arms are benchmarked one at a
# time, so they never contend, and an A/B is only honest if both binaries ran
# on the same cores.
#
# Layout is overridable by environment variable; defaults suit an 18-vCPU box:
#   VKP_CPUS_BACKEND=1,2,3,4 one core per backend process: the standalone
#                            backend takes the first, cluster masters the rest.
#                            They must not overlap — a standalone-vs-cluster
#                            round benchmarks one arm at a time, but sharing a
#                            core makes the idle side's cluster cron land on
#                            the busy side's server.
#   VKP_CPUS_PROXY=5-8       every proxy arm (taskset over the whole process)
#   VKP_CPUS_BENCH=10-17     exported for bench.sh --cpus-bench
set -euo pipefail

STATE_DIR="${VKP_BENCH_DIR:-/tmp/vkp-bench-env}"
CPUS_BACKEND="${VKP_CPUS_BACKEND:-1,2,3,4}"
CPUS_PROXY="${VKP_CPUS_PROXY:-5-8}"
CPUS_BENCH="${VKP_CPUS_BENCH:-10-17}"
BACKEND_PORT="${VKP_BACKEND_PORT:-6399}"
CLUSTER_BASE_PORT="${VKP_CLUSTER_BASE_PORT:-7000}"
PROXY_BASE_PORT="${VKP_PROXY_BASE_PORT:-6380}"

SERVER="$(command -v valkey-server || command -v redis-server || true)"
CLI="$(command -v valkey-cli || command -v redis-cli || true)"

usage() { sed -n '2,30p' "$0" | sed 's|^# \?||'; }

die() { echo "$*" >&2; exit 1; }

nth_cpu() {  # nth_cpu LIST INDEX -> a single core from a taskset-style list
  python3 - "$1" "$2" <<'PY'
import sys
spec, idx = sys.argv[1], int(sys.argv[2])
cores = []
for part in spec.split(","):
    if "-" in part:
        a, b = part.split("-")
        cores += list(range(int(a), int(b) + 1))
    else:
        cores.append(int(part))
print(cores[idx % len(cores)])
PY
}

# Lists live proxy pids, one per line. Reads /proc rather than asking ps or
# pkill, because both match on `comm` — the executable's basename truncated to
# 15 characters. An A/B run compares binaries copied to distinct names, and
# `m6-B-before.proxyd` has a comm of `m6-B-before.pro`, so `pkill -x proxyd`
# silently matched none of them. Four proxies ended up bound to port 6380 at
# once; SO_REUSEPORT split the traffic across all of them while bench.sh
# charged the CPU of exactly one, which is how an arm came out reading 40%
# cheaper than its identical twin. Match on argv[0] instead, which is the full
# path we launched.
#
# Zombies are skipped: the container's PID 1 is `sleep infinity` and never
# reaps, so dead proxies accumulate in /proc forever and a wait keyed on their
# presence would never finish. A zombie holds no sockets, so it is harmless.
live_proxies() {
  python3 - <<'PY'
import os
for pid in os.listdir("/proc"):
    if not pid.isdigit():
        continue
    try:
        argv0 = open(f"/proc/{pid}/cmdline").read().split("\0")[0]
        if not os.path.basename(argv0).endswith("proxyd"):
            continue
        raw = open(f"/proc/{pid}/stat").read()
        if raw[raw.rindex(") ") + 2] == "Z":
            continue
    except (OSError, ValueError, IndexError):
        continue
    print(pid)
PY
}

live_proxyd() { [[ -n "$(live_proxies)" ]]; }

# SIGTERM starts a graceful drain that runs for up to --shutdown-grace-ms, and
# for that whole window the old process is STILL BOUND TO ITS PORT. Under
# SO_REUSEPORT the kernel cheerfully load-balances new connections across the
# dying proxy and its replacement, so a benchmark started too soon measures a
# blend of two binaries while bench.sh charges the CPU of only one of them —
# which reads as one arm being dramatically cheaper than its twin. If the old
# proxy also outlived a cluster teardown, its backends are gone and its share
# of the connections answers "-ERR proxy: cluster topology unavailable".
# Both were observed. Wait for actual exit, then escalate.
wait_gone() {
  for _ in $(seq 1 100); do
    live_proxyd || return 0
    sleep 0.1
  done
  echo "proxyd did not exit on SIGTERM after 10s, sending SIGKILL" >&2
  for pid in $(live_proxies); do kill -KILL "${pid}" 2>/dev/null || true; done
  for _ in $(seq 1 50); do
    live_proxyd || return 0
    sleep 0.1
  done
  return 1
}

wait_pong() {  # wait_pong PORT
  for _ in $(seq 1 100); do
    [[ "$("${CLI}" -p "$1" ping 2>/dev/null || true)" == "PONG" ]] && return 0
    sleep 0.1
  done
  return 1
}

# PING is not readiness for a cluster-mode proxy: the listener is up before the
# workers have pulled CLUSTER SLOTS, and each worker holds its own topology, so
# a keyed command can still come back "-ERR proxy: cluster topology unavailable"
# after PONG. That surfaced as a benchmark failing in round 1 against a proxy
# that looked healthy. A keyed GET is the real gate — repeated, because the
# worker that answers is whichever one the connection lands on.
wait_ready() {  # wait_ready PORT
  wait_pong "$1" || return 1
  for _ in $(seq 1 100); do
    local ok=1
    for _probe in 1 2 3 4 5 6 7 8; do
      case "$("${CLI}" -p "$1" get vkp-bench-readiness 2>&1)" in
        *"topology unavailable"*|*"no topology"*|*BACKEND*|*unavailable*) ok=0; break ;;
      esac
    done
    [[ "${ok}" -eq 1 ]] && return 0
    sleep 0.1
  done
  return 1
}

cmd_up() {
  local want_cluster=0
  [[ "${1:-}" == "--cluster" ]] && want_cluster=1
  [[ -z "${SERVER}" || -z "${CLI}" ]] && die "need valkey-server/valkey-cli (or redis equivalents)"

  mkdir -p "${STATE_DIR}"
  if "${CLI}" -p "${BACKEND_PORT}" ping >/dev/null 2>&1; then
    echo "backend already up on ${BACKEND_PORT}"
  else
    local core
    core="$(nth_cpu "${CPUS_BACKEND}" 0)"
    taskset -c "${core}" "${SERVER}" --port "${BACKEND_PORT}" --save '' --appendonly no \
      --protected-mode no --daemonize yes --dir "${STATE_DIR}" \
      --logfile "${STATE_DIR}/backend-${BACKEND_PORT}.log"
    wait_pong "${BACKEND_PORT}" || die "backend never answered on ${BACKEND_PORT}"
    echo "backend  127.0.0.1:${BACKEND_PORT}  cpu ${core}"
  fi

  if [[ "${want_cluster}" -eq 1 ]]; then
    if "${CLI}" -p "${CLUSTER_BASE_PORT}" ping >/dev/null 2>&1; then
      echo "cluster already up on ${CLUSTER_BASE_PORT}+"
    else
      # No replicas: they cost a core each and serve no read traffic here.
      VKP_CLUSTER_DIR="${STATE_DIR}/cluster" \
        "$(dirname "$0")/cluster-up.sh" --masters 3 --replicas 0 \
        --base-port "${CLUSTER_BASE_PORT}" >"${STATE_DIR}/cluster-up.log" 2>&1 \
        || die "cluster-up failed, see ${STATE_DIR}/cluster-up.log"
    fi
    # cluster-up.sh does not pin; move each master onto its own core now that
    # they exist. Unpinned masters reintroduce exactly the noise we removed.
    # Index 0 belongs to the standalone backend; masters start at 1.
    local i=1
    for ((port = CLUSTER_BASE_PORT; port < CLUSTER_BASE_PORT + 3; port++)); do
      local pid core
      pid="$("${CLI}" -p "${port}" info server 2>/dev/null | sed -n 's/^process_id:\([0-9]*\).*/\1/p')"
      core="$(nth_cpu "${CPUS_BACKEND}" "${i}")"
      [[ -n "${pid}" ]] && taskset -cp "${core}" "${pid}" >/dev/null
      echo "cluster  127.0.0.1:${port}  cpu ${core}"
      i=$((i + 1))
    done
  fi
  echo
  echo "bench threads should use cpus ${CPUS_BENCH} (bench.sh --cpus-bench)"
}

cmd_proxy() {
  local name="$1" binary="$2"; shift 2
  [[ -x "${binary}" ]] || die "not executable: ${binary}"
  mkdir -p "${STATE_DIR}"

  # Ports are handed out in registration order; the cpuset is deliberately the
  # same for every arm (see the header).
  local index=0
  [[ -f "${STATE_DIR}/arms" ]] && index="$(wc -l < "${STATE_DIR}/arms")"
  local port=$((PROXY_BASE_PORT + index))
  local cpus="${CPUS_PROXY}"

  # Second line of defence behind wait_gone(): SO_REUSEPORT will not refuse a
  # bind that collides with a survivor, it will silently split the traffic.
  if "${CLI}" -p "${port}" ping >/dev/null 2>&1; then
    die "port ${port} already answers; something is still bound to it (run 'bench-env.sh down')"
  fi

  local nworkers
  nworkers="$(python3 -c "
import sys
spec='${cpus}'
n=0
for p in spec.split(','):
    n += (int(p.split('-')[1]) - int(p.split('-')[0]) + 1) if '-' in p else 1
print(n)")"

  # taskset picks the cpuset, --cpu-affinity gives each worker its own core
  # inside it. Both: without the pinning the workers float and two of them
  # share a core on some rounds, which reads as an 8% throughput swing.
  taskset -c "${cpus}" nohup "${binary}" -l "127.0.0.1:${port}" -w "${nworkers}" \
    --cpu-affinity --admin-listen "" "$@" > "${STATE_DIR}/proxy-${name}.log" 2>&1 &
  echo $! > "${STATE_DIR}/proxy-${name}.pid"
  wait_ready "${port}" || die "proxy '${name}' never became ready on ${port}, see ${STATE_DIR}/proxy-${name}.log"
  local pid
  pid="$(cat "${STATE_DIR}/proxy-${name}.pid")"
  printf '%s\t%s\t%s\t%s\t%s\n' "${name}" "127.0.0.1:${port}" "${pid}" "${cpus}" "${binary}" \
    >> "${STATE_DIR}/arms"
  echo "proxy    ${name}  127.0.0.1:${port}  pid ${pid}  cpus ${cpus}  ${nworkers} worker(s)"
}

cmd_arms() {
  # Proxy arms carry their pid so bench.sh can charge CPU time to them.
  local flags=("--cpus-bench" "${CPUS_BENCH}" "--arm" "direct=127.0.0.1:${BACKEND_PORT}")
  if [[ -f "${STATE_DIR}/arms" ]]; then
    while IFS=$'\t' read -r name addr pid _ _; do
      flags+=("--arm" "${name}=${addr}@${pid}")
    done < "${STATE_DIR}/arms"
  fi
  echo "${flags[@]}"
}

cmd_status() {
  echo "state dir: ${STATE_DIR}"
  [[ -f "${STATE_DIR}/arms" ]] && { echo; printf 'name\taddr\tpid\tcpus\tbinary\n'; cat "${STATE_DIR}/arms"; }
  echo
  for pid in $(live_proxies); do
    ps -o pid,psr,args -p "${pid}" --no-headers 2>/dev/null || true
  done
}

cmd_down() {
  for pid in $(live_proxies); do kill -TERM "${pid}" 2>/dev/null || true; done
  wait_gone || echo "warning: a proxyd survived SIGKILL; ports may still be shared" >&2
  if [[ -d "${STATE_DIR}/cluster" ]]; then
    VKP_CLUSTER_DIR="${STATE_DIR}/cluster" "$(dirname "$0")/cluster-down.sh" >/dev/null 2>&1 || true
  fi
  [[ -n "${CLI}" ]] && "${CLI}" -p "${BACKEND_PORT}" shutdown nosave >/dev/null 2>&1 || true
  rm -f "${STATE_DIR}/arms"
  echo "down"
}

case "${1:-}" in
  up) shift; cmd_up "$@" ;;
  proxy) shift; [[ $# -ge 2 ]] || die "usage: bench-env.sh proxy NAME BINARY [args...]"; cmd_proxy "$@" ;;
  arms) cmd_arms ;;
  status) cmd_status ;;
  down) cmd_down ;;
  -h|--help|"") usage ;;
  *) die "unknown command '$1'" ;;
esac
