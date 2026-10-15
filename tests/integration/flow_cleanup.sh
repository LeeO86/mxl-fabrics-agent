#!/usr/bin/env bash
# Mirror flows whose origin flow is gone, in a mirror domain that stays in use (host-01, 2026-10-15):
# two agents, tcp over loopback, three audio flows in one domain on A. Two origins end; B keeps the
# domain for the third. B logs what it would remove (dry run), then removes the flow nobody reads,
# keeps the one a reader maps until the reader is gone, and never touches the live flow.
#
# The reader check needs every process's /proc/<pid>/maps: run as root (CI) or with CAP_SYS_PTRACE.
# Without it B refuses every removal; the script checks that refusal and stops there, unless
# REQUIRE_READER_CHECK=1 (CI), which makes it a failure.
set -u -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$ROOT/build/mxl-fabrics-agent}"
WRITER="${WRITER:-$ROOT/build/mxl-pattern-writer}"
if [[ ! -x "$BIN" && -x /tmp/mfa-agent/mxl-fabrics-agent ]]; then
  BIN=/tmp/mfa-agent/mxl-fabrics-agent
  WRITER=/tmp/mfa-agent/mxl-pattern-writer
fi

fail() { echo "[flow-cleanup] FAIL: $*" >&2; exit 1; }
say() { echo "[flow-cleanup] $*"; }

command -v curl >/dev/null || fail "curl missing"
command -v python3 >/dev/null || fail "python3 missing"
[[ -x "$BIN" ]] || fail "agent binary missing: $BIN"
[[ -x "$WRITER" ]] || fail "writer missing: $WRITER"

BASE=$(mktemp -d /dev/shm/mfa-flowclean.XXXXXX)
A="$BASE/a"
B="$BASE/b"
mkdir -p "$A" "$B" "$BASE/state-a" "$BASE/state-b"
DOMAIN="c1c1c1c1-c1c1-4c1c-8c1c-c1c1c1c1c1c1"
F_LIVE="c2c2c2c2-c2c2-4c2c-8c2c-c2c2c2c2c2c2" # origin stays: the domain stays in use
F_GONE="c3c3c3c3-c3c3-4c3c-8c3c-c3c3c3c3c3c3" # origin ends, nobody reads the mirror
F_READ="c4c4c4c4-c4c4-4c4c-8c4c-c4c4c4c4c4c4" # origin ends, a reader maps the mirror
MIRROR="$B/mirror-$DOMAIN"
LOGA="$BASE/a.log"
LOGB="$BASE/b.log"
WEB_A=18295
WEB_B=18296
PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT

python3 "$ROOT/tests/integration/fake_nmos.py" 18991 18990 >"$BASE/nmos.log" 2>&1 &
PIDS+=($!)
sleep 0.3

PEERS_A='[{"host_id":"node-b","control_url":"http://127.0.0.1:18296/api/v1","local_fabric_addr":"127.0.0.1","remote_fabric_addr":"127.0.0.1","provider":"tcp"}]'
PEERS_B='[{"host_id":"node-a","control_url":"http://127.0.0.1:18295/api/v1","local_fabric_addr":"127.0.0.1","remote_fabric_addr":"127.0.0.1","provider":"tcp"}]'

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

# MIRROR_GRACE_S is long so that an origin seen inactive before it is gone does not take the 1.0 path
# (planMirror, origin_gone): this test is about flows no peer offers any more.
common=(
  DEFAULT_PROVIDER=tcp
  FABRIC_INTERFACE=127.0.0.1
  NMOS_ENABLE=true
  NMOS_DNS_SD=false
  NMOS_HOST_ADDRESS="$HOST_ADDR"
  NMOS_REGISTRY_ADDRESS=127.0.0.1
  NMOS_REGISTRY_PORT=18990
  NMOS_QUERY_ADDRESS=127.0.0.1
  NMOS_QUERY_PORT=18991
  LOCAL_NODE_CIDRS=127.0.0.0/8
  MIRROR_MODE=eager
  MIRROR_GRACE_S=120
  MIRROR_IDLE_GRACE_S=3
  RELEASE_GRACE_MS=800
  SCAN_INTERVAL_MS=400
  PEER_POLL_INTERVAL_MS=400
  NMOS_POLL_INTERVAL_MS=400
  TMPFS_RESERVE_MB=1
  LOG_LEVEL=info
)

env "${common[@]}" HOST_ID=node-a MXL_ROOT="$A" STATE_DIR="$BASE/state-a" WEB_PORT=$WEB_A NMOS_PORT=18432 \
  FABRIC_PORT_BASE=24000 PEERS="$PEERS_A" "$BIN" >"$LOGA" 2>&1 &
PIDS+=($!)
env "${common[@]}" HOST_ID=node-b MXL_ROOT="$B" STATE_DIR="$BASE/state-b" WEB_PORT=$WEB_B NMOS_PORT=18442 \
  FABRIC_PORT_BASE=24100 PEERS="$PEERS_B" "$BIN" >"$LOGB" 2>&1 &
PIDS+=($!)

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
# wait_for <seconds> <command...>: polls every 0.5 s.
wait_for() {
  local tries=$(($1 * 2))
  shift
  for ((i = 0; i < tries; i++)); do
    if "$@"; then
      return 0
    fi
    sleep 0.5
  done
  return 1
}
logged() { grep "\"event\":\"$1\"" "$LOGB" | grep -q "$2"; }
on_disk() { [[ -d "$MIRROR/$1.mxl-flow" ]]; }
gone() { [[ ! -d "$MIRROR/$1.mxl-flow" ]]; }
metric() { curl -sf --max-time 2 "http://127.0.0.1:$WEB_B/metrics" | grep "^mxl_fabrics_agent_$1" || true; }
dump() {
  echo "--- B ---"; grep -v '"level":"debug"' "$LOGB" | tail -n 40
  echo "--- mirrors B ---"; curl -sf "http://127.0.0.1:$WEB_B/api/v1/mirrors" || true
  echo; ls -la "$MIRROR" 2>/dev/null || true
}

wait_http "http://127.0.0.1:$WEB_A/livez" 150 || fail "agent A did not start"
wait_http "http://127.0.0.1:$WEB_B/livez" 150 || fail "agent B did not start"
# Dry run first, through the runtime configuration (an environment value could not be changed later).
curl -sf --max-time 2 -X PUT "http://127.0.0.1:$WEB_B/api/v1/config" -d '{"MIRROR_CLEANUP_DRY_RUN":true}' >/dev/null \
  || fail "could not set MIRROR_CLEANUP_DRY_RUN"
say "agents up, B in dry run"

"$WRITER" audio "$A/fc" "$DOMAIN" "$F_LIVE" 240 200 48 >"$BASE/writer-live.log" 2>&1 &
PIDS+=($!)
"$WRITER" audio "$A/fc" "$DOMAIN" "$F_GONE" 14 200 48 >"$BASE/writer-gone.log" 2>&1 &
GONE_WRITER=$!
PIDS+=($!)
"$WRITER" audio "$A/fc" "$DOMAIN" "$F_READ" 14 200 48 >"$BASE/writer-read.log" 2>&1 &
READ_WRITER=$!
PIDS+=($!)
all_mirrored() { on_disk "$F_LIVE" && on_disk "$F_GONE" && on_disk "$F_READ"; }
wait_for 20 all_mirrored || { dump; fail "the three flows were not mirrored"; }
say "three flows mirrored in one domain"

# A receiver on the flow that will be read: samples arrive in its mirror, and a reader maps it.
curl -sf --max-time 2 -X PUT http://127.0.0.1:18991/control \
  -H 'content-type: application/json' \
  -d "{\"master_enable\":true,\"mxl_domain_id\":\"$DOMAIN\",\"mxl_flow_id\":\"$F_READ\"}" >/dev/null
"$WRITER" samples "$MIRROR" "$F_READ" 180 0 >"$BASE/reader.log" 2>&1 &
READER=$!
PIDS+=($!)

# The two short origins end; MXL deletes a flow with its last writer. Gone from disk either way.
wait "$GONE_WRITER" 2>/dev/null || true
wait "$READ_WRITER" 2>/dev/null || true
for flow in "$F_GONE" "$F_READ"; do
  [[ -d "$A/fc/$flow.mxl-flow" ]] && rm -r "$A/fc/$flow.mxl-flow"
done
kill -0 "$READER" 2>/dev/null || { cat "$BASE/reader.log"; dump; fail "the reader of $F_READ did not start"; }
say "origins of two flows gone, a reader maps one mirror"

wait_for 15 logged mirror_flow_origin_missing "$F_GONE" || { dump; fail "origin_missing was not logged"; }
# Removals wait 30 s after start (peers known) and MIRROR_IDLE_GRACE_S.
wait_for 60 logged mirror_flow_would_remove "$F_GONE" || { dump; fail "dry run: mirror_flow_would_remove was not logged"; }
on_disk "$F_GONE" || fail "dry run removed $F_GONE"
on_disk "$F_READ" || fail "dry run removed $F_READ"
metric "mirror_state{" | grep -q "$F_GONE" || { metric "mirror_state{"; fail "no mirror_state for $F_GONE before removal"; }
say "dry run: logged, nothing removed"

curl -sf --max-time 2 -X PUT "http://127.0.0.1:$WEB_B/api/v1/config" -d '{"MIRROR_CLEANUP_DRY_RUN":false}' >/dev/null \
  || fail "could not clear MIRROR_CLEANUP_DRY_RUN"

readers_known=$(curl -sf --max-time 2 "http://127.0.0.1:$WEB_B/api/v1/mirrors" | python3 -c '
import json, sys
rows = [r for r in json.load(sys.stdin) if r.get("domain_id") == sys.argv[1]]
print("true" if rows and rows[0].get("readers_known") else "false")' "$DOMAIN")
if [[ "$readers_known" != true ]]; then
  wait_for 15 logged mirror_flow_kept_reader "$F_GONE" || { dump; fail "no refusal without a reader check"; }
  grep '"event":"mirror_flow_kept_reader"' "$LOGB" | grep "$F_GONE" | grep -q "reader check incomplete" \
    || { dump; fail "the refusal does not name the incomplete reader check"; }
  on_disk "$F_GONE" || fail "$F_GONE removed without a complete reader check"
  [[ "${REQUIRE_READER_CHECK:-0}" == 1 ]] && fail "the reader check is incomplete (run as root or with CAP_SYS_PTRACE)"
  say "reader check incomplete here: every removal refused (run as root for the rest)"
  say "passed"
  exit 0
fi

wait_for 15 gone "$F_GONE" || { dump; fail "$F_GONE was not removed"; }
logged mirror_flow_removed "$F_GONE" || { dump; fail "mirror_flow_removed was not logged for $F_GONE"; }
say "flow without a reader removed"

wait_for 15 logged mirror_flow_kept_reader "$F_READ" || { dump; fail "mirror_flow_kept_reader was not logged for $F_READ"; }
grep '"event":"mirror_flow_kept_reader"' "$LOGB" | grep "$F_READ" | grep -q "mapped by $READER(" \
  || { dump; fail "the refusal does not name the reader pid $READER"; }
on_disk "$F_READ" || fail "$F_READ removed while a reader maps it"
say "flow with a reader kept"

on_disk "$F_LIVE" || fail "the live flow was removed"
[[ -f "$MIRROR/domain_def.json" ]] || fail "the mirror domain was removed"
metric "mirror_state{" | grep -q "$F_GONE" && { metric "mirror_state{"; fail "the removed flow is still in mirror_state"; }
metric 'mirror_flows_removed_total{reason="origin_missing"} 1$' | grep -q . \
  || { metric mirror_flows; fail "mirror_flows_removed_total is not 1"; }
metric 'mirror_flows_refused_total{reason="origin_missing"}' | grep -q . || { metric mirror_flows; fail "no mirror_flows_refused_total"; }
say "live flow and domain kept, metrics counted"

kill -TERM "$READER" 2>/dev/null || true
wait "$READER" 2>/dev/null || true
wait_for 15 gone "$F_READ" || { dump; fail "$F_READ was not removed after its reader left"; }
logged mirror_flow_removed "$F_READ" || { dump; fail "mirror_flow_removed was not logged for $F_READ"; }
metric 'mirror_flows_removed_total{reason="origin_missing"} 2$' | grep -q . \
  || { metric mirror_flows; fail "mirror_flows_removed_total is not 2"; }
on_disk "$F_LIVE" || fail "the live flow was removed"
say "flow removed once its reader left"
say "passed"
