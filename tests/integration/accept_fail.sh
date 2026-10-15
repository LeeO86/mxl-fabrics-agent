#!/usr/bin/env bash
# A destination whose target fails to accept a connection, then both agents restarted together.
#
# 1. Connect failure on the source (the same MXL bug in the RC initiator) and accept failure (production, verbs: rdma_create_qp EINVAL ~1 s after a restart, then SIGSEGV). B runs
#    with tests/tools/accept_fault.cpp preloaded: the queues of the first connection its target accepts
#    cannot be opened, so the target's read fails. With MXL 218ddaa the next read of that target crashes
#    (docs/upstream-rctarget-accept-crash.md). B has to close the target, set up a new one and replicate.
# 2. Both agents stopped and started together, the mirror left on disk (production: host-01 logged
#    mirror_flow_origin_missing for a live flow 3 s after start). No origin_missing for the live flow: not in
#    the log, not in the mirror health.
set -u -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$ROOT/build/mxl-fabrics-agent}"
WRITER="${WRITER:-$ROOT/build/mxl-pattern-writer}"
FAULT="${FAULT:-$ROOT/build/libmfa-accept-fault.so}"

fail() { echo "[accept_fail] FAIL: $*" >&2; exit 1; }
say() { echo "[accept_fail] $*"; }

command -v curl >/dev/null || fail "curl missing"
command -v python3 >/dev/null || fail "python3 missing"
[[ -x "$BIN" ]] || fail "agent binary missing: $BIN"
[[ -x "$WRITER" ]] || fail "writer missing: $WRITER"
[[ -f "$FAULT" ]] || fail "fault library missing: $FAULT"

BASE=$(mktemp -d /dev/shm/mfa-accept.XXXXXX)
A="$BASE/a"
B="$BASE/b"
mkdir -p "$A" "$B" "$BASE/tmp-a" "$BASE/tmp-b"
DOMAIN_ID="c1c1c1c1-c1c1-4c1c-8c1c-c1c1c1c1c1c1"
FLOW_ID="c2c2c2c2-c2c2-4c2c-8c2c-c2c2c2c2c2c2"
MIRROR="$B/mirror-$DOMAIN_ID"
DATA="$MIRROR/$FLOW_ID.mxl-flow/data"
WEB_A=18111
WEB_B=18112
PIDS=()
APID=""
BPID=""

cleanup() {
  for pid in "${PIDS[@]:-}" $APID $BPID; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT

dump() {
  echo "--- A ---"; grep -v '"level":"debug"' "$BASE"/a*.log | tail -n 20
  echo "--- B ---"; grep -v '"level":"debug"' "$BASE"/b*.log | tail -n 40
  echo "--- replications B ---"; curl -sf --max-time 2 "http://127.0.0.1:$WEB_B/api/v1/replications" || true
}

python3 "$ROOT/tests/integration/fake_nmos.py" 18971 18970 >"$BASE/nmos.log" 2>&1 &
PIDS+=($!)
sleep 0.3

PEERS_A="[{\"host_id\":\"node-b\",\"control_url\":\"http://127.0.0.1:$WEB_B/api/v1\",\"local_fabric_addr\":\"127.0.0.1\",\"remote_fabric_addr\":\"127.0.0.1\",\"provider\":\"tcp\"}]"
PEERS_B="[{\"host_id\":\"node-a\",\"control_url\":\"http://127.0.0.1:$WEB_A/api/v1\",\"local_fabric_addr\":\"127.0.0.1\",\"remote_fabric_addr\":\"127.0.0.1\",\"provider\":\"tcp\"}]"

HOST_ADDR="$(hostname -I 2>/dev/null | awk '{print $1}')"
if [[ -z "$HOST_ADDR" || "$HOST_ADDR" == 127.* ]]; then
  HOST_ADDR="$(python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.connect(("192.0.2.1", 9))
print(s.getsockname()[0])
PY
)"
fi
[[ -n "$HOST_ADDR" && "$HOST_ADDR" != 127.* ]] || fail "need a non-loopback IPv4 for NMOS_HOST_ADDRESS"

common=(
  DEFAULT_PROVIDER=tcp
  FABRIC_INTERFACE=127.0.0.1
  NMOS_ENABLE=true
  NMOS_DNS_SD=false
  NMOS_HOST_ADDRESS="$HOST_ADDR"
  NMOS_REGISTRY_ADDRESS=127.0.0.1
  NMOS_REGISTRY_PORT=18970
  NMOS_QUERY_ADDRESS=127.0.0.1
  NMOS_QUERY_PORT=18971
  LOCAL_NODE_CIDRS=127.0.0.0/8
  MIRROR_MODE=eager
  SCAN_INTERVAL_MS=400
  PEER_POLL_INTERVAL_MS=400
  NMOS_POLL_INTERVAL_MS=400
  TMPFS_RESERVE_MB=1
  LOG_LEVEL=info
)

start_a() { # run, extra env...
  local run=$1
  shift
  env "${common[@]}" "$@" TMPDIR="$BASE/tmp-a" HOST_ID=node-a MXL_ROOT="$A" WEB_PORT=$WEB_A NMOS_PORT=18272 FABRIC_PORT_BASE=23810 PEERS="$PEERS_A" \
    "$BIN" >>"$BASE/a$run.log" 2>&1 &
  APID=$!
}
start_b() { # run, extra env...
  local run=$1
  shift
  env "${common[@]}" "$@" TMPDIR="$BASE/tmp-b" HOST_ID=node-b MXL_ROOT="$B" WEB_PORT=$WEB_B NMOS_PORT=18282 FABRIC_PORT_BASE=23910 PEERS="$PEERS_B" \
    "$BIN" >>"$BASE/b$run.log" 2>&1 &
  BPID=$!
}
wait_http() {
  local url="$1" tries="${2:-50}"
  for ((i = 0; i < tries; i++)); do
    if curl -sf --max-time 1 "$url" >/dev/null; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}
mirror_head() { # the mirror's head index (0 before the first grain)
  python3 - "$DATA" <<'PY'
import struct, sys
try:
    with open(sys.argv[1], "rb") as f:
        buf = f.read(208)
    print(struct.unpack_from("<Q", buf, 200)[0] if len(buf) == 208 else 0)
except OSError:
    print(0)
PY
}
mirror_moves() { # the mirror's head moves within 10 s
  local first next
  first=$(mirror_head)
  for ((i = 0; i < 50; i++)); do
    sleep 0.2
    next=$(mirror_head)
    if [[ "$next" != 0 && "$next" != 18446744073709551615 && "$next" != "$first" ]]; then
      return 0
    fi
  done
  return 1
}

"$WRITER" video "$A/video" "$DOMAIN_ID" "$FLOW_ID" 120 200 >"$BASE/writer.log" 2>&1 &
PIDS+=($!)
sleep 1

# 1. A's first connect to B fails (its initiator throws in fi_connect), and the first connection B's
#    target accepts fails.
start_a 1 LD_PRELOAD="$FAULT" MFA_FAULT_CONNECT_PORTS=23910-23999
start_b 1 LD_PRELOAD="$FAULT" MFA_FAULT_PORTS=23910-23999
wait_http "http://127.0.0.1:$WEB_A/livez" 150 || fail "agent A did not start"
wait_http "http://127.0.0.1:$WEB_B/livez" 150 || fail "agent B did not start"
say "agents up, A with the connect fault, B with the accept fault"
curl -sf --max-time 2 -X PUT http://127.0.0.1:18971/control \
  -d "{\"master_enable\":true,\"mxl_domain_id\":\"$DOMAIN_ID\",\"mxl_flow_id\":\"$FLOW_ID\"}" >/dev/null
for ((i = 0; i < 75; i++)); do grep -q '"event":"accept_fault_injected"' "$BASE/b1.log" && break; sleep 0.2; done
grep -q '"event":"accept_fault_injected"' "$BASE/b1.log" || { dump; fail "the fault was not injected (no connection accepted?)"; }
for ((i = 0; i < 50; i++)); do grep -q '"event":"fabric_target_closed"' "$BASE/b1.log" && break; sleep 0.2; done
grep -q '"event":"fabric_target_closed"' "$BASE/b1.log" || { dump; fail "B did not close the failed target"; }
sleep 1
kill -0 "$BPID" 2>/dev/null || { wait "$BPID"; code=$?; dump; fail "B died after the failed accept (exit $code)"; }
for ((i = 0; i < 75; i++)); do grep -q '"event":"replication_restarted"' "$BASE/b1.log" && break; sleep 0.2; done
grep -q '"event":"replication_restarted"' "$BASE/b1.log" || { dump; fail "B did not set the target up again"; }
mirror_moves || { dump; fail "no grains on B's mirror after the new target"; }
kill -0 "$BPID" 2>/dev/null || { dump; fail "B died"; }
grep -q '"event":"connect_fault_injected"' "$BASE/a1.log" || { dump; fail "the connect fault was not injected on A"; }
grep -q '"event":"fabric_initiator_closed"' "$BASE/a1.log" || { dump; fail "A did not close the failed initiator"; }
kill -0 "$APID" 2>/dev/null || { dump; fail "A died after the failed connect"; }
say "failed connect: initiator closed, A alive; failed accept: target closed, B alive; new ones set up, the mirror gets grains"

# 2. Both agents stopped and started together; B keeps the mirror on disk (MXL_CLEANUP_ON_EXIT=false).
kill -TERM "$APID" "$BPID"
wait "$APID" "$BPID" 2>/dev/null || true
[[ -f "$DATA" ]] || fail "the mirror flow did not stay on disk"
start_a 2
start_b 2
wait_http "http://127.0.0.1:$WEB_B/livez" 150 || fail "agent B did not start again"
# Watch B's mirror health for 8 s: the flow is live on A all along, so origin_missing must never show.
for ((i = 0; i < 40; i++)); do
  health=$(curl -sf --max-time 1 "http://127.0.0.1:$WEB_B/api/v1/mirrors" || true)
  if [[ "$health" == *origin_missing* ]]; then
    echo "$health"
    dump
    fail "health shows origin_missing for a live flow after the restart"
  fi
  sleep 0.2
done
if grep -q '"event":"mirror_flow_origin_missing"' "$BASE/b2.log"; then
  dump
  fail "mirror_flow_origin_missing for a live flow after the restart"
fi
mirror_moves || { dump; fail "no grains on B's mirror after the restart"; }
say "restart together: no origin_missing for the live flow, the mirror gets grains"
say "passed"
