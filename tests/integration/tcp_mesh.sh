#!/usr/bin/env bash
# Two agents, tcp over loopback, one writer, one fake NMOS receiver.
set -u -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$ROOT/build/mxl-fabrics-agent}"
WRITER="${WRITER:-$ROOT/build/mxl-pattern-writer}"
if [[ ! -x "$BIN" && -x /tmp/mfa-agent/mxl-fabrics-agent ]]; then
  BIN=/tmp/mfa-agent/mxl-fabrics-agent
  WRITER=/tmp/mfa-agent/mxl-pattern-writer
fi

fail() { echo "[tcp-mesh] FAIL: $*" >&2; exit 1; }
say() { echo "[tcp-mesh] $*"; }

command -v curl >/dev/null || fail "curl missing"
command -v python3 >/dev/null || fail "python3 missing"
[[ -x "$BIN" ]] || fail "agent binary missing: $BIN"
[[ -x "$WRITER" ]] || fail "writer missing: $WRITER"

BASE=$(mktemp -d /dev/shm/mfa-mesh.XXXXXX)
A="$BASE/a"
B="$BASE/b"
mkdir -p "$A/src" "$B"
DOMAIN_ID="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
FLOW_ID="11111111-1111-4111-8111-111111111111"
LOGA="$BASE/a.log"
LOGB="$BASE/b.log"
PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT

python3 "$ROOT/tests/integration/fake_nmos.py" 18971 18970 >"$BASE/nmos.log" 2>&1 &
PIDS+=($!)
sleep 0.3

PEERS_A='[{"host_id":"node-b","control_url":"http://127.0.0.1:18096/api/v1","local_fabric_addr":"127.0.0.1","remote_fabric_addr":"127.0.0.1","provider":"tcp"}]'
PEERS_B='[{"host_id":"node-a","control_url":"http://127.0.0.1:18095/api/v1","local_fabric_addr":"127.0.0.1","remote_fabric_addr":"127.0.0.1","provider":"tcp"}]'

common=(
  DEFAULT_PROVIDER=tcp
  FABRIC_INTERFACE=127.0.0.1
  NMOS_ENABLE=true
  NMOS_REGISTRY_ADDRESS=127.0.0.1
  NMOS_REGISTRY_PORT=18970
  NMOS_QUERY_ADDRESS=127.0.0.1
  NMOS_QUERY_PORT=18971
  LOCAL_NODE_IDS=bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb
  MIRROR_MODE=eager
  RELEASE_GRACE_MS=800
  MIRROR_GRACE_S=3
  SCAN_INTERVAL_MS=400
  PEER_POLL_INTERVAL_MS=400
  NMOS_POLL_INTERVAL_MS=400
  TMPFS_RESERVE_MB=1
  LOG_LEVEL=info
)

env "${common[@]}" HOST_ID=node-a MXL_ROOT="$A" WEB_PORT=18095 NMOS_PORT=18232 FABRIC_PORT_BASE=23600 PEERS="$PEERS_A" \
  "$BIN" >"$LOGA" 2>&1 &
PIDS+=($!)
env "${common[@]}" HOST_ID=node-b MXL_ROOT="$B" WEB_PORT=18096 NMOS_PORT=18242 FABRIC_PORT_BASE=23700 PEERS="$PEERS_B" \
  "$BIN" >"$LOGB" 2>&1 &
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

wait_http http://127.0.0.1:18095/livez || fail "agent A did not start"
wait_http http://127.0.0.1:18096/livez || fail "agent B did not start"
say "agents up"

"$WRITER" "$A/src" "$FLOW_ID" 2000 >"$BASE/writer.log" 2>&1 &
PIDS+=($!)

mirror_ready=0
for ((i = 0; i < 40; i++)); do
  if curl -sf --max-time 1 http://127.0.0.1:18096/api/v1/mirrors | grep -q "$FLOW_ID"; then
    mirror_ready=1
    break
  fi
  sleep 0.25
done
[[ "$mirror_ready" == 1 ]] || { tail -n 40 "$LOGB"; fail "eager mirror was not created before activation"; }
say "eager mirror exists before IS-05 activation"

curl -sf --max-time 2 -X PUT http://127.0.0.1:18971/control \
  -H 'content-type: application/json' \
  -d "{\"master_enable\":true,\"mxl_domain_id\":\"$DOMAIN_ID\",\"mxl_flow_id\":\"$FLOW_ID\"}" >/dev/null

if ! "$WRITER" read "$B/mirror-$DOMAIN_ID" "$FLOW_ID" 25; then
  echo "--- A ---"; tail -n 30 "$LOGA"
  echo "--- B ---"; tail -n 40 "$LOGB"
  echo "--- mirrors ---"; curl -sf http://127.0.0.1:18096/api/v1/mirrors || true
  echo "--- reps ---"; curl -sf http://127.0.0.1:18096/api/v1/replications || true
  echo "--- demand ---"; curl -sf http://127.0.0.1:18096/api/v1/demand || true
  fail "replicated grain index did not match"
fi
say "grain indices match"

curl -sf --max-time 2 -X PUT http://127.0.0.1:18971/control \
  -H 'content-type: application/json' \
  -d '{"master_enable":false,"mxl_domain_id":null,"mxl_flow_id":null}' >/dev/null
released=0
for ((i = 0; i < 30; i++)); do
  body=$(curl -sf --max-time 1 http://127.0.0.1:18095/api/v1/replications || echo '[]')
  if ! grep -q "$FLOW_ID" <<<"$body"; then
    released=1
    break
  fi
  sleep 0.3
done
[[ "$released" == 1 ]] || fail "source did not release the target after grace"
say "target released"

# Peer down: stop A, B should report the peer down, then A returns and replication can resume.
kill -TERM "${PIDS[1]}" 2>/dev/null || true
wait "${PIDS[1]}" 2>/dev/null || true
unset 'PIDS[1]'
down=0
for ((i = 0; i < 25; i++)); do
  if curl -sf --max-time 1 http://127.0.0.1:18096/api/v1/peers | grep -q '"up":false'; then
    down=1
    break
  fi
  sleep 0.3
done
[[ "$down" == 1 ]] || fail "peer_down was not reported"
say "peer down reported"

env "${common[@]}" HOST_ID=node-a MXL_ROOT="$A" WEB_PORT=18095 NMOS_PORT=18232 FABRIC_PORT_BASE=23600 PEERS="$PEERS_A" \
  "$BIN" >"$LOGA" 2>&1 &
PIDS+=($!)
curl -sf --max-time 2 -X PUT http://127.0.0.1:18971/control \
  -H 'content-type: application/json' \
  -d "{\"master_enable\":true,\"mxl_domain_id\":\"$DOMAIN_ID\",\"mxl_flow_id\":\"$FLOW_ID\"}" >/dev/null
"$WRITER" "$A/src" "$FLOW_ID" 800 >"$BASE/writer2.log" 2>&1 &
PIDS+=($!)
if ! "$WRITER" read "$B/mirror-$DOMAIN_ID" "$FLOW_ID" 25; then
  fail "replication did not resume"
fi
say "replication resumed"

curl -sf --max-time 2 -X PUT http://127.0.0.1:18971/control \
  -H 'content-type: application/json' \
  -d "{\"master_enable\":true,\"mxl_domain_id\":\"$DOMAIN_ID\",\"mxl_flow_id\":\"99999999-9999-4999-8999-999999999999\"}" >/dev/null
stale=0
for ((i = 0; i < 20; i++)); do
  if curl -sf --max-time 1 http://127.0.0.1:18096/api/v1/demand | grep -q stale_reference; then
    stale=1
    break
  fi
  sleep 0.3
done
[[ "$stale" == 1 ]] || fail "stale_reference was not reported"
say "stale reference reported"
say "passed"
