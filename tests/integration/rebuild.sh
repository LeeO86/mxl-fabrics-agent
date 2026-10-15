#!/usr/bin/env bash
# Mirror rebuilds: the test hook POST /api/v1/mirrors/{domain}/{flow}/rebuild (off by default, 403) and an
# origin that comes back with another flow definition under the same flow id. A reader of the mirror that
# re-opens on MXL_ERR_FLOW_INVALID has to get grains again after each rebuild.
set -u -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$ROOT/build/mxl-fabrics-agent}"
WRITER="${WRITER:-$ROOT/build/mxl-pattern-writer}"

fail() { echo "[rebuild] FAIL: $*" >&2; exit 1; }
say() { echo "[rebuild] $*"; }

command -v curl >/dev/null || fail "curl missing"
command -v python3 >/dev/null || fail "python3 missing"
[[ -x "$BIN" ]] || fail "agent binary missing: $BIN"
[[ -x "$WRITER" ]] || fail "writer missing: $WRITER"

BASE=$(mktemp -d /dev/shm/mfa-rebuild.XXXXXX)
A="$BASE/a"
B="$BASE/b"
mkdir -p "$A" "$B"
DOMAIN_ID="b1b1b1b1-b1b1-4b1b-8b1b-b1b1b1b1b1b1"
FLOW_ID="b2b2b2b2-b2b2-4b2b-8b2b-b2b2b2b2b2b2"
MIRROR="$B/mirror-$DOMAIN_ID"
DATA="$MIRROR/$FLOW_ID.mxl-flow/data"
LOGA="$BASE/a.log"
LOGB="$BASE/b.log"
WEB_A=18097
WEB_B=18098
PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT

dump() {
  echo "--- A ---"; grep -v '"level":"debug"' "$LOGA" | tail -n 20
  echo "--- B ---"; grep -v '"level":"debug"' "$LOGB" | tail -n 30
  echo "--- mirrors B ---"; curl -sf "http://127.0.0.1:$WEB_B/api/v1/mirrors" || true
  echo "--- reader ---"; cat "$BASE/reader.log" 2>/dev/null || true
}

python3 "$ROOT/tests/integration/fake_nmos.py" 18961 18960 >"$BASE/nmos.log" 2>&1 &
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
  NMOS_REGISTRY_PORT=18960
  NMOS_QUERY_ADDRESS=127.0.0.1
  NMOS_QUERY_PORT=18961
  LOCAL_NODE_CIDRS=127.0.0.0/8
  MIRROR_MODE=eager
  SCAN_INTERVAL_MS=400
  PEER_POLL_INTERVAL_MS=400
  NMOS_POLL_INTERVAL_MS=400
  TMPFS_RESERVE_MB=1
  LOG_LEVEL=info
)

# Only B opens the hook.
env "${common[@]}" HOST_ID=node-a MXL_ROOT="$A" WEB_PORT=$WEB_A NMOS_PORT=18252 FABRIC_PORT_BASE=23800 PEERS="$PEERS_A" \
  "$BIN" >"$LOGA" 2>&1 &
PIDS+=($!)
env "${common[@]}" MIRROR_ADMIN_REBUILD=true HOST_ID=node-b MXL_ROOT="$B" WEB_PORT=$WEB_B NMOS_PORT=18262 FABRIC_PORT_BASE=23900 \
  PEERS="$PEERS_B" "$BIN" >"$LOGB" 2>&1 &
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
wait_http "http://127.0.0.1:$WEB_A/livez" 150 || fail "agent A did not start"
wait_http "http://127.0.0.1:$WEB_B/livez" 150 || fail "agent B did not start"
say "agents up"

inode() { stat -c %i "$DATA" 2>/dev/null || echo 0; }
rate() { python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["grain_rate"]["numerator"])' "$MIRROR/$FLOW_ID.mxl-flow/flow_def.json" 2>/dev/null || echo -; }
rebuilds() { curl -sf --max-time 2 "http://127.0.0.1:$WEB_B/metrics" | awk '/^mxl_fabrics_agent_mirror_rebuilds_total\{/ {print $2; exit}'; }
post() { curl -s -o "$BASE/post.json" -w '%{http_code}' --max-time 5 -X POST "http://127.0.0.1:$1/api/v1/mirrors/$2/$3/rebuild"; }
# Waits for the reader's n-th re-open and a grain after it.
reader_back() {
  local n="$1"
  for ((i = 0; i < 75; i++)); do
    if grep -q "re-open $n " "$BASE/reader.log" && grep -A5 "re-open $n " "$BASE/reader.log" | grep -q "first grain"; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

# A 50 grains/s writer that exits cleanly after 25 s (MXL deletes its flow), a receiver on B.
PATTERN_VIDEO_RATE=50 "$WRITER" video "$A/video" "$DOMAIN_ID" "$FLOW_ID" 25 200 >"$BASE/writer1.log" 2>&1 &
WPID=$!
PIDS+=($WPID)
sleep 1
curl -sf --max-time 2 -X PUT http://127.0.0.1:18961/control \
  -d "{\"master_enable\":true,\"mxl_domain_id\":\"$DOMAIN_ID\",\"mxl_flow_id\":\"$FLOW_ID\"}" >/dev/null
for ((i = 0; i < 50 && $(inode) == 0; i++)); do sleep 0.2; done
[[ $(inode) != 0 ]] || { dump; fail "no mirror flow on B"; }
"$WRITER" follow "$MIRROR" "$FLOW_ID" 60 >"$BASE/reader.log" 2>&1 &
RPID=$!
PIDS+=($RPID)
for ((i = 0; i < 75; i++)); do grep -q "first grain" "$BASE/reader.log" && break; sleep 0.2; done
grep -q "first grain" "$BASE/reader.log" || { dump; fail "the reader got no grain from the mirror"; }
say "mirror live, reader reading"

# The hook: off on A (403 before anything else), bad ids 400, an unknown flow 404.
code=$(post $WEB_A "$DOMAIN_ID" "$FLOW_ID")
[[ "$code" == 403 ]] || fail "hook on A (MIRROR_ADMIN_REBUILD unset): http $code, expected 403"
code=$(post $WEB_B "not-a-uuid" "$FLOW_ID")
[[ "$code" == 400 ]] || fail "hook with a bad id: http $code, expected 400"
code=$(post $WEB_B "$DOMAIN_ID" "99999999-9999-4999-8999-999999999999")
[[ "$code" == 404 ]] || fail "hook for an unknown flow: http $code, expected 404"
code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 2 -X PUT "http://127.0.0.1:$WEB_A/api/v1/config" -d '{"MIRROR_ADMIN_REBUILD":true}')
[[ "$code" == 400 ]] || fail "PUT /api/v1/config with MIRROR_ADMIN_REBUILD: http $code, expected 400 (environment only)"
code=$(post $WEB_A "$DOMAIN_ID" "$FLOW_ID")
[[ "$code" == 403 ]] || fail "hook on A after the PUT: http $code, expected 403"
say "hook guarded: 403 off, 400 bad id, 404 unknown flow, setting environment-only (PUT 400)"

before=$(inode)
code=$(post $WEB_B "$DOMAIN_ID" "$FLOW_ID")
[[ "$code" == 200 ]] || { cat "$BASE/post.json"; dump; fail "hook on B: http $code"; }
after=$(inode)
[[ "$after" != 0 && "$after" != "$before" ]] || { cat "$BASE/post.json"; fail "hook: mirror inode $before -> $after"; }
reader_back 1 || { dump; fail "the reader did not get grains again after the API rebuild"; }
for ((i = 0; i < 25; i++)); do grep -q '"event":"mirror_rebuilt".*"reason":"api"' "$LOGB" && break; sleep 0.2; done
grep -q '"event":"mirror_rebuilt".*"reason":"api"' "$LOGB" || { dump; fail "no mirror_rebuilt with reason api"; }
[[ "$(rebuilds)" == 1 ]] || fail "mirror_rebuilds_total is $(rebuilds), expected 1"
say "API rebuild: inode $before -> $after, reader re-opened and reads, mirror_rebuilds_total 1"

# The writer exits (MXL deletes the origin flow) and comes back at 25 grains/s under the same flow id:
# the mirror is rebuilt from the new definition and the reader re-opens again.
wait "$WPID" 2>/dev/null || true
sleep 1
before=$(inode)
PATTERN_VIDEO_RATE=25 "$WRITER" video "$A/video" "$DOMAIN_ID" "$FLOW_ID" 60 200 >"$BASE/writer2.log" 2>&1 &
PIDS+=($!)
for ((i = 0; i < 50 && "$(rate)" != 25; i++)); do sleep 0.2; done
[[ "$(rate)" == 25 ]] || { dump; fail "the mirror kept $(rate) grains/s after the origin came back at 25"; }
[[ "$(inode)" != "$before" ]] || fail "changed origin: the mirror inode stayed $before"
grep -q '"event":"mirror_origin_changed"' "$LOGB" || fail "no mirror_origin_changed"
reader_back 2 || { dump; fail "the reader did not get grains again after the origin changed"; }
grep -q '"event":"mirror_rebuilt".*"reason":"origin_config_changed"' "$LOGB" || { dump; fail "no mirror_rebuilt with reason origin_config_changed"; }
say "origin back at 25 grains/s: mirror rebuilt (origin_config_changed), reader re-opened and reads"
say "passed"
