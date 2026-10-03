#!/usr/bin/env bash
# Start one agent, wait until it is registered, then SIGTERM.
# The process must exit 143, DELETE its NMOS node, and remove only its own mirrors.
set -u -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${BIN:-$ROOT/build/mxl-fabrics-agent}"
if [[ ! -x "$BIN" && -x /tmp/mfa-agent/mxl-fabrics-agent ]]; then
  BIN=/tmp/mfa-agent/mxl-fabrics-agent
fi

fail() { echo "[shutdown] FAIL: $*" >&2; exit 1; }
say() { echo "[shutdown] $*"; }

command -v curl >/dev/null || fail "curl missing"
command -v python3 >/dev/null || fail "python3 missing"
[[ -x "$BIN" ]] || fail "agent binary missing: $BIN"

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

BASE=$(mktemp -d /dev/shm/mfa-shutdown.XXXXXX)
MXL="$BASE/mxl"
DOMAIN="aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
FOREIGN="bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbc"
mkdir -p "$MXL/mirror-$DOMAIN" "$MXL/mirror-$FOREIGN" "$MXL/local-domain"
cat >"$MXL/mirror-$DOMAIN/domain_def.json" <<EOF
{"id":"$DOMAIN","x-mxl-fabrics-agent":{"mirror":true,"source_host_id":"node-b","owner_host_id":"node-a"}}
EOF
cat >"$MXL/mirror-$FOREIGN/domain_def.json" <<EOF
{"id":"$FOREIGN","x-mxl-fabrics-agent":{"mirror":true,"source_host_id":"node-c","owner_host_id":"node-b"}}
EOF
echo '{"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc"}' >"$MXL/local-domain/domain_def.json"

PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill -TERM "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT

python3 "$ROOT/tests/integration/fake_nmos.py" 18981 18980 >"$BASE/nmos.log" 2>&1 &
PIDS+=($!)
sleep 0.3

env \
  HOST_ID=node-a \
  MXL_ROOT="$MXL" \
  MXL_DOMAIN_SCAN_PATH="$MXL" \
  DEFAULT_PROVIDER=tcp \
  FABRIC_INTERFACE=127.0.0.1 \
  NMOS_ENABLE=true \
  NMOS_DNS_SD=false \
  NMOS_HOST_ADDRESS="$HOST_ADDR" \
  NMOS_SEED=shutdown-agent \
  NMOS_LABEL=shutdown-agent \
  NMOS_TAGS='{"urn:x-srf:function":["fabrics"]}' \
  NMOS_REGISTRY_ADDRESS=127.0.0.1 \
  NMOS_REGISTRY_PORT=18980 \
  NMOS_QUERY_ADDRESS=127.0.0.1 \
  NMOS_QUERY_PORT=18981 \
  MXL_CLEANUP_ON_EXIT=true \
  SHUTDOWN_TIMEOUT_S=8 \
  WEB_PORT=18195 \
  NMOS_PORT=18332 \
  FABRIC_PORT_BASE=23900 \
  SCAN_INTERVAL_MS=400 \
  LOG_LEVEL=info \
  STATE_DIR="$BASE/state" \
  "$BIN" >"$BASE/agent.log" 2>&1 &
AGENT=$!
PIDS+=($AGENT)

for ((i = 0; i < 50; i++)); do
  if curl -sf --max-time 1 http://127.0.0.1:18195/readyz >/dev/null; then
    break
  fi
  if ! kill -0 "$AGENT" 2>/dev/null; then
    echo "--- agent ---"; tail -n 80 "$BASE/agent.log" || true
    fail "agent exited before ready"
  fi
  sleep 0.2
done
curl -sf --max-time 1 http://127.0.0.1:18195/readyz >/dev/null || {
  echo "--- agent ---"; tail -n 80 "$BASE/agent.log" || true
  echo "--- status ---"; curl -s --max-time 1 http://127.0.0.1:18195/statusz || true
  fail "readyz did not become ready"
}
say "ready"

NODE_ID="$(curl -sf --max-time 2 http://127.0.0.1:18981/x-nmos/query/v1.3/nodes | python3 -c 'import json,sys; ids=[n.get("id","") for n in json.load(sys.stdin) if n.get("id")!="bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"]; print(ids[0] if ids else "")')"
[[ -n "$NODE_ID" ]] || fail "registered node was not visible on the query API"
say "registered $NODE_ID"

curl -sf --max-time 2 http://127.0.0.1:18195/api/v1/config/export >"$BASE/export.json"
python3 - <<'PY' "$BASE/export.json"
import json, sys
doc = json.load(open(sys.argv[1]))
assert doc["MXL_CLEANUP_ON_EXIT"] == "true"
assert doc["NMOS_DNS_SD"] == "false"
assert "NMOS_SEED" in doc
assert "passphrase" not in json.dumps(doc).lower()
PY

# Drop the agent from the trap list so we can observe its exit status.
PIDS=("${PIDS[0]}")
kill -TERM "$AGENT"
set +e
wait "$AGENT"
code=$?
set -e
[[ "$code" == 143 ]] || { tail -n 40 "$BASE/agent.log"; fail "exit code $code, expected 143"; }
say "exit 143"

[[ ! -e "$MXL/mirror-$DOMAIN" ]] || fail "own mirror directory was not removed"
[[ -d "$MXL/mirror-$FOREIGN" ]] || fail "foreign mirror was removed"
[[ -f "$MXL/local-domain/domain_def.json" ]] || fail "another function domain was removed"
curl -sf --max-time 2 http://127.0.0.1:18980/deleted | grep -q "$NODE_ID" || {
  echo "--- deleted ---"; curl -s --max-time 1 http://127.0.0.1:18980/deleted || true
  echo "--- agent ---"; tail -n 40 "$BASE/agent.log" || true
  fail "registry did not record a DELETE for $NODE_ID"
}
say "node deleted and own mirror removed"

kill -TERM "${PIDS[0]}" 2>/dev/null || true
wait "${PIDS[0]}" 2>/dev/null || true
PIDS=()
trap - EXIT
say "ok"
