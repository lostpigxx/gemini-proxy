#!/usr/bin/env bash
# Runs the M6 standard scenario set against two or more labelled endpoints and
# prints a comparison report.
#
#   ./scripts/bench.sh --arm direct=127.0.0.1:6399 --arm proxy=127.0.0.1:6380
#   ./scripts/bench.sh --matrix full --rounds 5 --out /tmp/bench-after \
#                      --arm a=127.0.0.1:6380 --arm b=127.0.0.1:6381
#
# Why arms instead of "start a proxy and measure it": every number this project
# has had to walk back came from comparing runs taken minutes or days apart. So
# bench.sh owns no processes at all — you start whatever you want to compare,
# label the endpoints, and it interleaves them arm-by-arm inside each scenario,
# seconds apart.
#
# Three measurement traps this harness closes, all of them things that have
# already produced a wrong number in this project:
#
#  1. GET hit rate. `-t get -r 1000000` against an unpopulated server is all
#     misses, and a `$-1` reply is a quarter the bytes of a `-d 32` hit: 4.00M
#     rps vs 1.54M on the same box, same second. This is the real source of the
#     "bimodal throughput" blamed on core placement in M3/M4/M5. Every run here
#     is preceded by a fill pass and the resulting dbsize is printed in the
#     report, so the hit rate is on the record instead of being assumed.
#  2. rps quantisation. redis-benchmark derives its reported rps from a 250 ms
#     progress tick, so a 3 s run can only land on ~8% steps — coarser than
#     anything M6 is trying to measure. bench.sh times the run itself and
#     reports wall-clock rps; the tool's own figure is kept in results.tsv as
#     `rps_reported` for comparison.
#  3. An unpinned load generator outruns nothing. See scripts/bench-env.sh.
#
# And one trap it cannot close, so it measures around it instead: redis-
# benchmark is a closed loop, so at `-c 50 -P 1` throughput is just
# clients/latency and both arms read the same ~363k — the client saturates
# first. Worse, the proxy *beats* a direct connection once connections
# outnumber what one redis thread likes to poll (c=200 P=1: 522k vs 374k),
# because it funnels them onto one batched backend socket. Throughput ratio
# against direct therefore measures the backend's socket handling, not the
# proxy's cost. `label=host:port@pid` adds the honest number: proxy CPU
# microseconds per request, read from /proc/<pid>/stat around each run.
#
# Reads: results.tsv (machine-readable, one row per run) and report.md.
# `--baseline OLD/results.tsv` prints an advisory diff against an earlier run —
# advisory because it is exactly the cross-round comparison described above.
set -euo pipefail

ARMS=()
ROUNDS=3
MATRIX=quick
OUT=""
BASELINE=""
THREADS=4
KEYSPACE=1000000
WARMUP=1
CPUS_BENCH=""

usage() { sed -n '2,25p' "$0" | sed 's|^# \?||'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --arm) ARMS+=("$2"); shift 2 ;;
    --cpus-bench) CPUS_BENCH="$2"; shift 2 ;;
    --rounds) ROUNDS="$2"; shift 2 ;;
    --matrix) MATRIX="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --baseline) BASELINE="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --keyspace) KEYSPACE="$2"; shift 2 ;;
    --no-warmup) WARMUP=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ "${#ARMS[@]}" -lt 1 ]]; then
  echo "need at least one --arm label=host:port" >&2
  usage >&2
  exit 2
fi

BENCH="$(command -v valkey-benchmark || command -v redis-benchmark || true)"
CLI="$(command -v valkey-cli || command -v redis-cli || true)"
if [[ -z "${BENCH}" ]]; then
  echo "need valkey-benchmark or redis-benchmark on PATH" >&2
  exit 1
fi
if ! command -v python3 >/dev/null; then
  echo "need python3 (report aggregation)" >&2
  exit 1
fi

# Confining the load generator to its own cores is what makes runs repeatable
# here: with it and the backend pinned, four consecutive `-P 16` runs return
# identical rps. See scripts/bench-env.sh for the layout and the history.
PIN=()
if [[ -n "${CPUS_BENCH}" ]]; then
  command -v taskset >/dev/null || { echo "--cpus-bench needs taskset" >&2; exit 1; }
  PIN=(taskset -c "${CPUS_BENCH}")
fi

OUT="${OUT:-/tmp/vkp-bench-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "${OUT}"
RESULTS="${OUT}/results.tsv"
LOGDIR="${OUT}/raw"
mkdir -p "${LOGDIR}"

# cmd  pipeline  datasize  clients  requests
#
# `requests` is not a knob: redis-benchmark reports rps quantised by its own
# timer, and at `-P 16` a 300k-request run finishes in a quarter second and
# reads back as the same round number for every configuration. Each row is
# sized to run for at least a second or two on this class of machine.
case "${MATRIX}" in
  quick)
    # M3's "baseline A" used `-c 50 -P 1`. It is not in here, on purpose: at 50
    # connections the closed-loop client tops out near 363k and reads the same
    # for every arm, and it swings 232k..363k between rounds *on the direct
    # arm too* — an instrument that noisy cannot see anything M6 will do. The
    # c=200 rows measure the same regime and hold to 0.3%. `full` keeps the old
    # scenario for continuity with the M3/M4 tables.
    SCENARIOS=(
      "SET 1 32 200 2000000"
      "GET 1 32 200 2000000"
      "SET 16 32 50 5000000"
      "GET 16 32 50 5000000"
    )
    ;;
  full)
    SCENARIOS=(
      "SET 1 32 50 1000000"
      "GET 1 32 50 1000000"
      "SET 1 32 200 2000000"
      "GET 1 32 200 2000000"
      "SET 4 32 50 2000000"
      "GET 4 32 50 2000000"
      "SET 16 32 50 5000000"
      "GET 16 32 50 5000000"
      "SET 1 512 200 2000000"
      "GET 1 512 200 2000000"
      "SET 16 512 50 2000000"
      "GET 16 512 50 2000000"
      "SET 16 32 200 5000000"
      "GET 16 32 200 5000000"
    )
    ;;
  pipeline)
    # One dimension at a time: how the per-request cost amortises with depth.
    SCENARIOS=(
      "GET 1 32 50 1000000"
      "GET 2 32 50 1000000"
      "GET 4 32 50 2000000"
      "GET 8 32 50 3000000"
      "GET 16 32 50 5000000"
      "GET 32 32 50 5000000"
    )
    ;;
  *)
    if [[ -f "${MATRIX}" ]]; then
      mapfile -t SCENARIOS < <(grep -v '^[[:space:]]*\(#\|$\)' "${MATRIX}")
    else
      echo "unknown matrix '${MATRIX}' (quick|full|pipeline|<file>)" >&2
      exit 2
    fi
    ;;
esac

declare -A ARM_ADDR
declare -A ARM_PID
ARM_ORDER=()
for spec in "${ARMS[@]}"; do
  label="${spec%%=*}"
  addr="${spec#*=}"
  pid=""
  if [[ "${addr}" == *@* ]]; then
    pid="${addr##*@}"
    addr="${addr%@*}"
  fi
  if [[ "${label}" == "${spec}" || -z "${addr}" ]]; then
    echo "malformed --arm '${spec}', want label=host:port[@pid]" >&2
    exit 2
  fi
  if [[ -n "${pid}" && ! -r "/proc/${pid}/stat" ]]; then
    echo "arm '${label}': no /proc/${pid}/stat" >&2
    exit 2
  fi
  ARM_ADDR["${label}"]="${addr}"
  ARM_PID["${label}"]="${pid}"
  ARM_ORDER+=("${label}")
done

# Fail loudly now rather than reporting an arm as "0 rps" later.
for label in "${ARM_ORDER[@]}"; do
  addr="${ARM_ADDR[${label}]}"
  host="${addr%:*}"
  port="${addr##*:}"
  if [[ -n "${CLI}" ]]; then
    if [[ "$("${CLI}" -h "${host}" -p "${port}" ping 2>/dev/null || true)" != "PONG" ]]; then
      echo "arm '${label}' at ${addr} does not answer PING" >&2
      exit 1
    fi
  fi
done

printf 'round\tarm\tcmd\tpipeline\tsize\tclients\trequests\trps\trps_reported\tavg\tp50\tp95\tp99\tcpu_us_per_req\n' \
  > "${RESULTS}"

# Total CPU ticks (utime+stime, all threads) of a process. Fields 14/15 of
# /proc/pid/stat, counted past the comm field because a process name may
# contain spaces or parentheses.
cpu_ticks() {
  [[ -z "$1" || ! -r "/proc/$1/stat" ]] && { echo ""; return; }
  python3 - "$1" <<'PY'
import sys
try:
    raw = open(f"/proc/{sys.argv[1]}/stat").read()
except OSError:
    print(""); raise SystemExit
fields = raw[raw.rindex(")") + 2:].split()
print(int(fields[11]) + int(fields[12]))  # utime, stime (field 14, 15)
PY
}
HZ="$(getconf CLK_TCK 2>/dev/null || echo 100)"

# Runs one scenario against one arm and appends a TSV row. redis-benchmark's
# --csv is parsed instead of its human output: the "====== SET ======" banner
# is not always emitted, and the latency block's column order has moved
# between versions.
run_one() {
  local round="$1" label="$2" cmd="$3" pipe="$4" size="$5" clients="$6" reqs="$7"
  local addr="${ARM_ADDR[${label}]}"
  local host="${addr%:*}" port="${addr##*:}"
  local tag="r${round}-${label}-${cmd}-P${pipe}-d${size}-c${clients}"
  local log="${LOGDIR}/${tag}.csv"

  local t0 t1 c0 c1 cpu_us=""
  c0="$(cpu_ticks "${ARM_PID[${label}]}")"
  t0="$(date +%s%N)"
  "${PIN[@]}" "${BENCH}" -h "${host}" -p "${port}" -t "${cmd,,}" -n "${reqs}" -c "${clients}" \
    -P "${pipe}" -d "${size}" -r "${KEYSPACE}" --threads "${THREADS}" --csv \
    > "${log}" 2>"${LOGDIR}/${tag}.err" || {
      echo "  ${label}: benchmark failed, see ${LOGDIR}/${tag}.err" >&2
      return 1
    }
  t1="$(date +%s%N)"
  c1="$(cpu_ticks "${ARM_PID[${label}]}")"
  if [[ -n "${c0}" && -n "${c1}" ]]; then
    cpu_us="$(python3 -c "print(f'{(${c1} - ${c0}) / ${HZ} * 1e6 / ${reqs}:.3f}')")"
  fi

  python3 - "${log}" "${round}" "${label}" "${cmd}" "${pipe}" "${size}" "${clients}" "${reqs}" \
    "$((t1 - t0))" "${cpu_us}" >> "${RESULTS}" <<'PY'
import csv, sys
path, round_, label, cmd, pipe, size, clients, reqs, elapsed_ns = sys.argv[1:10]
cpu_us = sys.argv[10] if len(sys.argv) > 10 else ""
with open(path, newline="") as fh:
    rows = [r for r in csv.reader(fh) if r and r[0] != "test"]
if not rows:
    sys.exit(f"no csv rows in {path}")
# "test","rps","avg","min","p50","p95","p99","max"
_t, rps_reported, avg, _min, p50, p95, p99 = rows[-1][:7]
# Wall clock beats the tool's own figure (250 ms tick, see the header). It
# folds in client connect/teardown, a fixed tens-of-ms that is identical for
# every arm and negligible at these run lengths.
rps = int(reqs) / (int(elapsed_ns) / 1e9)
print("\t".join([round_, label, cmd, pipe, size, clients, reqs,
                 f"{rps:.2f}", rps_reported, avg, p50, p95, p99, cpu_us]))
PY
}

echo "matrix=${MATRIX} rounds=${ROUNDS} arms=${ARM_ORDER[*]} out=${OUT}"
echo

DBSIZE="n/a"
if [[ "${WARMUP}" -eq 1 ]]; then
  # Fill, then warm. The fill is the one that matters: a partially populated
  # keyspace gives GET a hit rate nobody wrote down, and the difference between
  # all-hit and all-miss is 2.6x on this box. 4x the keyspace in random SETs is
  # ~98% coverage (coupon collector); the dbsize check below is the real proof.
  #
  # Sizes are the largest in the matrix so the values left behind are the ones
  # the GET scenarios will read back.
  echo "fill: ${KEYSPACE} keys ..."
  maxsize=32
  for scenario in "${SCENARIOS[@]}"; do
    read -r _c _p s _cl _n <<< "${scenario}"
    (( s > maxsize )) && maxsize="${s}"
  done
  addr="${ARM_ADDR[${ARM_ORDER[0]}]}"
  "${PIN[@]}" "${BENCH}" -h "${addr%:*}" -p "${addr##*:}" -t set -n $((KEYSPACE * 4)) \
    -c 50 -P 16 -d "${maxsize}" -r "${KEYSPACE}" --threads "${THREADS}" --csv >/dev/null 2>&1 || true

  if [[ -n "${CLI}" ]]; then
    a="${ARM_ADDR[${ARM_ORDER[0]}]}"
    DBSIZE="$("${CLI}" -h "${a%:*}" -p "${a##*:}" dbsize 2>/dev/null || echo "n/a")"
    if [[ "${DBSIZE}" =~ ^[0-9]+$ ]]; then
      pct=$((100 * DBSIZE / KEYSPACE))
      echo "  dbsize=${DBSIZE} (${pct}% of keyspace)"
      if [[ "${pct}" -lt 95 ]]; then
        echo "  WARNING: keyspace under-filled; GET numbers will mix hits and misses" >&2
      fi
    else
      echo "  dbsize unavailable (cluster mode rejects DBSIZE); GET hit rate unverified"
    fi
  fi

  echo "warmup ..."
  for label in "${ARM_ORDER[@]}"; do
    addr="${ARM_ADDR[${label}]}"
    "${PIN[@]}" "${BENCH}" -h "${addr%:*}" -p "${addr##*:}" -t get -n 200000 -c 50 -P 16 \
      -d 32 -r "${KEYSPACE}" --threads "${THREADS}" --csv >/dev/null 2>&1 || true
  done
fi

for ((round = 1; round <= ROUNDS; round++)); do
  for scenario in "${SCENARIOS[@]}"; do
    read -r cmd pipe size clients reqs <<< "${scenario}"
    printf 'round %d  %-3s P=%-2s d=%-4s c=%-3s n=%-8s ' \
      "${round}" "${cmd}" "${pipe}" "${size}" "${clients}" "${reqs}"
    # Arms adjacent in time, inside the scenario: this is the whole point.
    for label in "${ARM_ORDER[@]}"; do
      printf '%s ' "${label}"
      run_one "${round}" "${label}" "${cmd}" "${pipe}" "${size}" "${clients}" "${reqs}"
    done
    printf '\n'
  done
done

echo
python3 - "${RESULTS}" "${BASELINE}" "${DBSIZE}" "${KEYSPACE}" <<'PY' | tee "${OUT}/report.md"
import statistics, sys
from collections import OrderedDict

def load(path):
    rows = []
    with open(path) as fh:
        header = fh.readline().rstrip("\n").split("\t")
        for line in fh:
            rows.append(dict(zip(header, line.rstrip("\n").split("\t"))))
    return rows

def key(r):
    return (r["cmd"], int(r["pipeline"]), int(r["size"]), int(r["clients"]))

METRICS = ("rps", "avg", "p50", "p95", "p99", "cpu_us_per_req")

def collect(rows):
    """scenario -> arm -> {metric: median across rounds}"""
    acc = OrderedDict()
    for r in rows:
        arm = acc.setdefault(key(r), OrderedDict()).setdefault(r["arm"], {})
        for m in METRICS:
            v = r.get(m, "")
            if v not in ("", None):
                arm.setdefault(m, []).append(float(v))
    return OrderedDict(
        (k, OrderedDict((a, {m: statistics.median(v) for m, v in ms.items()})
                        for a, ms in arms.items()))
        for k, arms in acc.items()
    )

rows = load(sys.argv[1])
data = collect(rows)
arms = list(OrderedDict.fromkeys(r["arm"] for r in rows))
rounds = len({r["round"] for r in rows})
ref = arms[0]

dbsize = sys.argv[3] if len(sys.argv) > 3 else "n/a"
keyspace = sys.argv[4] if len(sys.argv) > 4 else "?"

print(f"# bench report\n")
print(f"arms: {', '.join(arms)} — reference arm is `{ref}`; "
      f"median of {rounds} round(s), arms interleaved within each scenario.")
print(f"keyspace {keyspace}, dbsize after fill {dbsize} — GET is a hit unless "
      f"these disagree. rps is wall-clock, not redis-benchmark's 250 ms tick.\n")

head = ["scenario"] + [f"{a} rps" for a in arms]
if len(arms) > 1:
    head += [f"{a}/{ref}" for a in arms[1:]]
head += [f"{a} p50" for a in arms] + [f"{a} p99" for a in arms]
print("| " + " | ".join(head) + " |")
print("|" + "---|" * len(head))

for (cmd, pipe, size, clients), byarm in data.items():
    name = f"{cmd} P={pipe} d={size} c={clients}"
    cells = [name]
    cells += [f"{byarm[a]['rps']:,.0f}" if a in byarm else "-" for a in arms]
    if len(arms) > 1:
        for a in arms[1:]:
            if a in byarm and ref in byarm and byarm[ref]["rps"]:
                cells.append(f"{100 * byarm[a]['rps'] / byarm[ref]['rps']:.1f}%")
            else:
                cells.append("-")
    cells += [f"{byarm[a]['p50']:.3f}" if a in byarm else "-" for a in arms]
    cells += [f"{byarm[a]['p99']:.3f}" if a in byarm else "-" for a in arms]
    print("| " + " | ".join(cells) + " |")

# The number an optimisation actually moves. Throughput here is set by the
# backend and the closed-loop client; CPU per request is the proxy's own cost
# and nothing else's.
cpu_arms = [a for a in arms
            if any("cpu_us_per_req" in b.get(a, {}) for b in data.values())]
if cpu_arms:
    print("\n## proxy CPU per request (µs, all worker threads)\n")
    head = ["scenario"] + [f"{a}" for a in cpu_arms]
    if len(cpu_arms) > 1:
        head += [f"{a}/{cpu_arms[0]}" for a in cpu_arms[1:]]
    print("| " + " | ".join(head) + " |")
    print("|" + "---|" * len(head))
    for k, byarm in data.items():
        cells = [f"{k[0]} P={k[1]} d={k[2]} c={k[3]}"]
        base = byarm.get(cpu_arms[0], {}).get("cpu_us_per_req")
        for a in cpu_arms:
            v = byarm.get(a, {}).get("cpu_us_per_req")
            cells.append(f"{v:.3f}" if v is not None else "-")
        for a in cpu_arms[1:]:
            v = byarm.get(a, {}).get("cpu_us_per_req")
            cells.append(f"{100 * v / base:.1f}%" if v is not None and base else "-")
        print("| " + " | ".join(cells) + " |")

# Spread across rounds, so a reader can tell a 3% win from this machine's noise.
print("\n## per-round spread (rps, min..max over rounds)\n")
spread = OrderedDict()
for r in rows:
    spread.setdefault((key(r), r["arm"]), []).append(float(r["rps"]))
print("| scenario | arm | min | max | spread |")
print("|---|---|---|---|---|")
for (k, arm), vals in spread.items():
    lo, hi = min(vals), max(vals)
    pct = (hi - lo) / lo * 100 if lo else 0.0
    print(f"| {k[0]} P={k[1]} d={k[2]} c={k[3]} | {arm} | {lo:,.0f} | {hi:,.0f} | {pct:.1f}% |")

baseline = sys.argv[2] if len(sys.argv) > 2 else ""
if baseline:
    old = collect(load(baseline))
    print(f"\n## vs baseline `{baseline}` (advisory)\n")
    print("Taken in a different round, so machine state is folded into every number "
          "below. Treat a delta smaller than the per-round spread above as noise.\n")
    print("| scenario | arm | old rps | new rps | delta |")
    print("|---|---|---|---|---|")
    for k, byarm in data.items():
        for a, ms in byarm.items():
            o = old.get(k, {}).get(a)
            if not o:
                continue
            d = 100 * (ms["rps"] - o["rps"]) / o["rps"] if o["rps"] else 0.0
            print(f"| {k[0]} P={k[1]} d={k[2]} c={k[3]} | {a} | "
                  f"{o['rps']:,.0f} | {ms['rps']:,.0f} | {d:+.1f}% |")
PY

echo
echo "results: ${RESULTS}"
echo "report:  ${OUT}/report.md"
