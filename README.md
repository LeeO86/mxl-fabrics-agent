# mxl-fabrics-agent

Per-host agent that makes MXL flows on one machine readable on another. Media functions keep their own NMOS nodes. A controller connects a receiver with ordinary IS-05 (`mxl_domain_id`, `mxl_flow_id`). When that domain is not local, the agent on the receiver's host pulls the flow over the MXL 1.1 Fabrics API (`verbs` for RoCEv2, `tcp` as fallback) and writes a mirror domain next to the local ones.

The agent does not PATCH IS-05 and does not register senders or receivers. It registers one Node, with no devices, so other agents can find it.

```bash
docker build -f docker/Dockerfile -t mxl-fabrics-agent .
```

## Ports

Under host networking these defaults share one port space with the sibling containers.

| Container | Port | Purpose |
| --- | --- | --- |
| mxl-decklink | 8080 | UI, REST, health, metrics |
| mxl-decklink | 3212, 3213 | NMOS Node API, websocket |
| mxl-st2110-gateway | 8080 (`node.http_port`) | IS-04, IS-05, UI, metrics. Collides with decklink; the gateway spec proposes 8090 until that default changes |
| mxl-fabrics-agent | 8095 | UI, `/api/v1`, health, metrics |
| mxl-fabrics-agent | 3232, 3233 | NMOS Node API, websocket |
| mxl-fabrics-agent | 23500–23599 | fabric target ports (TCP or RDMA CM) |

Two agents on one host (the loopback demo) must use different `WEB_PORT`, `NMOS_PORT`, and `FABRIC_PORT_BASE`.

## What a media function needs

1. Receivers resolve `mxl_domain_id` by scanning domains under the MXL root. Mirror domains are siblings of local domains (`mirror-<source-domain-id>`). The id is the `id` inside `domain_def.json`, not the directory name. A function that moves to another host should remove its old domain: while two hosts hold the same flow, a destination replicates from the one that is being written and logs `origin_conflict`.
2. If a flow is not present yet, the reader retries with backoff instead of failing permanently.
3. Readers tolerate a flow that exists but has no new grains yet.
4. Media functions do not build with `MXL_ENABLE_FABRICS_OFI`.
5. Readers on a destination should sit at least one grain behind the mirror head (`mxl_fabrics_agent_replication_lag_grains`).

mxl-decklink and mxl-st2110-gateway reject an IS-05 activation whose domain is not on disk yet. Use the default `MIRROR_MODE=eager` with them. `on-demand` is for readers that retry domain discovery.

## Clocks and NICs

Grain indices are TAI. Every host that writes, replicates, or reads a flow needs a disciplined `CLOCK_TAI` (PTP or chrony with the kernel TAI offset, currently 37 s). A zero offset is logged and exported as `mxl_fabrics_agent_tai_offset_seconds`.

`verbs` is the libfabric verbs provider. It is the same setting for:

- Intel E810, with the `irdma` kernel module in RoCEv2 mode
- NVIDIA/Mellanox ConnectX (CX-4 and later), with the `mlx5` kernel provider

Check the device with `ibv_devinfo`. Put the fabric addresses in the peer map. On a switch, leave the per-peer addresses empty and set `FABRIC_INTERFACE` to the fabric NIC address. On a switchless full mesh, each pair has its own subnet:

```json
"PEERS": [
  {"host_id": "node-b", "local_fabric_addr": "192.168.12.1", "remote_fabric_addr": "192.168.12.2", "provider": "verbs"},
  {"host_id": "node-c", "local_fabric_addr": "192.168.13.1", "remote_fabric_addr": "192.168.13.3", "provider": "verbs"}
]
```

One origin flow sent to both peers uses one reader and initiator per local address. MTU must match on each link. `PROVIDER_FALLBACK=tcp` retries a link on TCP if verbs setup fails and marks that replication `fallback: true`.

The agent watches each peer's link: the carrier of the local interface that holds `local_fabric_addr` (or `FABRIC_INTERFACE`), and that some interface holds the address at all. A dead link is `link_down` with the reason in `/api/v1/peers` and `/api/v1/replications`, `mxl_fabrics_agent_peer_up{peer,interface}` is 0, and the log has one `peer_link_down` line. The destination then drops its targets for that peer and retries nothing; when the link is back (`peer_link_up`) it sets up fresh ones. `mxl_fabrics_agent_peer_rdma_retransmits_total{peer,device}` is the RDMA device's `RetransSegs` (irdma): a lossy link shows there first. Flows are not relayed over a third host.

RoCEv2 on ConnectX can use the Mellanox QoS example in the MXL Fabrics getting-started guide (DSCP 26, PFC priority 3). E810 RoCEv2 is selected in the `irdma` driver; use the same DSCP/PFC policy on the switch or the direct link.

## tmpfs size

Ring depth is domain-wide: `history_duration` (nanoseconds) times the grain or sample rate. A mirror copies `options.json`, so the depth matches the origin.

| Flow | Approximate payload | 200 ms history |
| --- | --- | --- |
| 1080p50 v210 | ~5.3 MB/frame | ~10 frames, ~53 MB |
| 2160p50 v210 | ~21 MB/frame | ~10 frames, ~210 MB |
| 1080p50 v210a | fill + 10-bit key | about 1.6× the v210 ring |
| 16 ch 48 kHz float | 64 B/sample | ~480 k samples, ~31 MB |
| `video/smpte291` | 4096 B/grain | ~10 grains, ~40 KB |

Keep `TMPFS_RESERVE_MB` (default 512) free. A mirror that would cross that line is not created and is reported as `insufficient_space`.

```
tmpfs size ≈ local flows + (mirrored flows × payload × ring depth) + reserve
```

## Loopback demo

The checked-in integration test is the single-machine demo: two MXL roots, two agents, the `tcp` provider, a pattern writer, and a small IS-04/IS-05 stand-in.

```bash
LD_LIBRARY_PATH=/opt/mxl/lib:/usr/local/lib FI_PROVIDER=tcp tests/integration/tcp_mesh.sh
```

It checks that the eager mirror exists before the receiver is activated, that grain indices match, that disabling the receiver releases the target after `RELEASE_GRACE_MS`, that stopping the source is reported as peer down, that replication resumes, that a flow id the source no longer has is `stale_reference`, that a 1080p50 flow with a 200 ms ring arrives at 50 grains/s, that a 2-channel 48 kHz flow written in 1 ms batches arrives at 48,000 samples/s, that a flow its writer creates again (new inode) replicates again, and that a destination that goes away costs the source a few log lines, not one per pass.

`docker/docker-compose.demo.yaml` is the same shape in containers, plus Prometheus and Grafana. `docker/docker-compose.host.yaml` is one host of a real mesh: host networking, `/dev/infiniband`, `IPC_LOCK`, unlimited memlock, and the MXL root mounted read-write.

## Three-host mesh

Use `docker-compose.host.yaml` on each machine. `HOST_ID` is the node name. Example for host A (`node-a`), with B and C on their own direct links:

```yaml
environment:
  HOST_ID: node-a
  MXL_ROOT: /Volumes/mxl
  DEFAULT_PROVIDER: verbs
  PEERS: |
    [{"host_id":"node-b","local_fabric_addr":"192.168.12.1","remote_fabric_addr":"192.168.12.2","provider":"verbs"},
     {"host_id":"node-c","local_fabric_addr":"192.168.13.1","remote_fabric_addr":"192.168.13.3","provider":"verbs"}]
```

Host B swaps the local and remote addresses on the A link and adds its link to C. Activate a receiver on B with A's `mxl_domain_id` and `mxl_flow_id`:

```bash
curl -X PATCH -H 'content-type: application/json' \
  -d '{"master_enable":true,"activation":{"mode":"activate_immediate"},
       "transport_params":[{"mxl_domain_id":"<domain>","mxl_flow_id":"<flow>"}]}' \
  "http://<receiver-host>:<nmos-port>/x-nmos/connection/v1.2/single/receivers/<id>/staged"
curl -X POST "http://<receiver-host>:<nmos-port>/x-nmos/connection/v1.2/single/receivers/<id>/activate"
```

With eager mode the mirror domain already exists. Grains show up in `/api/v1/replications` on both agents.

## Kubernetes

`deploy/mxl-fabrics-agent.yaml` is a DaemonSet: `hostNetwork`, `IPC_LOCK`, the MXL root hostPath, and a ConfigMap selected by `HOST_ID` (downward API node name). Set `LOCAL_NODE_CIDRS` in that per-node file to the node's pod CIDR (`kubectl get node <name> -o jsonpath='{.spec.podCIDR}'`) so NMOS Nodes on the pod network of the same machine count as local. RDMA device access reuses the generic device plugin pattern from mxl-decklink, exposing `/dev/infiniband`. `kubectl exec` into the agent and run `ibv_devices` to see the RDMA devices it can use. `deploy/monitoring/` has a ServiceMonitor example. The Grafana dashboard is `deploy/grafana/mxl-fabrics-agent.json`.

## Configuration

Every setting is an environment variable. An optional JSON file (`AGENT_CONFIG_FILE`) is the file layer. Precedence is environment, then file, then the default. Unknown environment variables are ignored. An invalid value exits 78 with a one-line JSON error. There are no secret settings, so logs and `GET /api/v1/config/export` contain the whole configuration.

Within one layer, a canonical name wins over its alias. If both are set and differ, the process exits 78. The environment still beats the file when the two layers disagree.

State the process writes for itself lives under `STATE_DIR` (default `/config`). When `AGENT_CONFIG_FILE` is unset and that directory can be created, configuration saves go to `$STATE_DIR/agent.json`. Mirror domains are MXL data under the scan path, not application state.

| Key | Default | Meaning |
| --- | --- | --- |
| `HOST_ID` | hostname | Fabrics peer identity. Also the NMOS node id seed when `NMOS_SEED` is unset |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | Parent of domain directories, including `mirror-*`. Alias: `MXL_ROOT` |
| `STATE_DIR` | `/config` | Writable directory for the saved configuration |
| `AGENT_CONFIG_FILE` | `$STATE_DIR/agent.json` when that directory is writable | JSON file layer |
| `SCAN_INTERVAL_MS` | 2000 | Full rescan interval |
| `MIRROR_MODE` | `eager` | `eager` or `on-demand` |
| `MIRROR_INCLUDE_DOMAINS` / `MIRROR_INCLUDE_FLOWS` | empty | Extra allow lists. Empty means no extra filter |
| `MIRROR_EXCLUDE_DOMAINS` / `MIRROR_EXCLUDE_FLOWS` | empty | Deny lists |
| `MIRROR_GRACE_S` | 10 | Keep a mirror after the origin disappears |
| `TMPFS_RESERVE_MB` | 512 | Free space left on the MXL filesystem |
| `MXL_CLEANUP_ON_EXIT` | false | On shutdown, remove mirror domains this agent owns. Alias: `CLEANUP_MIRRORS_ON_EXIT`. With false the mirror flows stay, and readers on the host keep reading them after a restart |
| `DEFAULT_PROVIDER` | `verbs` | `verbs` or `tcp` |
| `PROVIDER_FALLBACK` | empty | `tcp` to retry a failed verbs link |
| `FABRIC_INTERFACE` | empty | Default local fabric address |
| `FABRIC_PORT_BASE` / `FABRIC_PORT_COUNT` | 23500 / 100 | Target port pool |
| `PEERS` | `[]` | JSON array of peer links |
| `PEER_POLL_INTERVAL_MS` | 2000 | Peer inventory poll |
| `RELEASE_GRACE_MS` | 2000 | Delay before releasing a target |
| `NMOS_ENABLE` | true | Register this node and observe the registry |
| `NMOS_SEED` | empty | UUIDv5 input for the node id. Unset keeps UUIDv5(`HOST_ID`) |
| `NMOS_LABEL` | `HOST_ID` | Node label. This agent has no device |
| `NMOS_TAGS` | `{}` | JSON object of tag name to string array, added to the node. The host-id tag is always set |
| `NMOS_DNS_SD` | false | `false` disables DNS-SD browse and mDNS advertisement. `true` needs Avahi; the platform does not use it |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | IP literal announced as the node href, `api.endpoints[].host`, and the service href. Not a hostname, `0.0.0.0`, or loopback |
| `NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT` | empty / 3210 | Registration API. Empty with `NMOS_DNS_SD=false` means no registry |
| `NMOS_QUERY_ADDRESS` / `NMOS_QUERY_PORT` | registry address / registry port + 1 | Query API |
| `NMOS_POLL_INTERVAL_MS` | 1000 | How often IS-05 `/active` is read |
| `NMOS_PORT` | 3232 | Node API. The WebSocket listener is `NMOS_PORT+1` |
| `LOCAL_NODE_IDS` / `LOCAL_NODE_HOSTNAMES` | empty | Extra local-node rules |
| `LOCAL_NODE_CIDRS` | empty | Pod CIDR of this node when media functions use the pod network |
| `WEB_PORT` | 8095 | UI, `/api/v1`, health, metrics |
| `WEB_ENABLE` | true | `false` hides the UI and `PUT /api/v1/config`. Health, metrics, and `/api/v1` stay up |
| `RT_PRIORITY` / `CPU_AFFINITY` | 0 / empty | Fabric thread scheduling. `RT_PRIORITY` (SCHED_FIFO) needs `SYS_NICE` in the container's capabilities (`--cap-add SYS_NICE`, Kubernetes `capabilities.add`); the image raises it from the binary's file capabilities. Keep it below 50: the kernel's threaded IRQs run at SCHED_FIFO 50, and at equal priority a NIC IRQ thread waits for the agent's pass (the platform uses 49) |
| `LOG_LEVEL` | `info` | `error`, `warn`, `info`, or `debug` |
| `METRICS_PER_FLOW` | true | Drop per-flow metric labels when false |
| `SHUTDOWN_TIMEOUT_S` | 10 | SIGTERM budget. The process then exits 143 |

`HOST_ID` is the fabrics peer id. It is not the address in the NMOS href. That address is `NMOS_HOST_ADDRESS`.

## Exit codes

| Code | When |
| --- | --- |
| 0 | The process left `main` without a termination signal. A SIGTERM shutdown is 143 |
| 75 | A listen port could not be bound (`WEB_PORT`, `NMOS_PORT`, or the WebSocket on `NMOS_PORT+1`), or no fabric provider is usable |
| 78 | Invalid configuration, or the MXL scan path is not a directory |
| 143 | SIGTERM or SIGINT, including when shutdown exceeds `SHUTDOWN_TIMEOUT_S` |

On SIGTERM the agent closes its fabric connections, releases MXL readers, removes its node from the registry, then, when `MXL_CLEANUP_ON_EXIT=true`, releases its writers and deletes only mirror directories it owns. With `false` it does not release its mirror writers: MXL deletes a flow when its last writer is released, and the next start re-opens the same flows. Another function's domain and another agent's mirrors are left in place.

## HTTP API

All of these are on `WEB_PORT`.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/livez` | Process is up |
| GET | `/readyz` | Scanner is running and, when a registry is configured, this node is registered |
| GET | `/metrics` | Prometheus text, prefix `mxl_fabrics_agent_` |
| GET | `/statusz` | Short JSON status |
| GET | `/` | Admin UI, unless `WEB_ENABLE=false` |
| GET | `/api/v1/info` | Version, host id, boot id |
| GET | `/api/v1/inventory` | Local domains and flows |
| GET | `/api/v1/mirrors` | Mirror domains |
| GET | `/api/v1/demand` | Local IS-05 demand |
| GET | `/api/v1/replications` | Active replications |
| POST | `/api/v1/replications` | Peer asks this host to send a flow |
| DELETE | `/api/v1/replications/{id}/targets/{dest}` | Peer releases a target |
| GET | `/api/v1/peers` | Peer status: control plane `up`, fabric `link_up`, `link_error`, `fabric_interface` |
| POST | `/api/v1/peers/{id}/test` | Control-plane and local target check |
| GET | `/api/v1/events` | Server-sent events |
| GET | `/api/v1/config` | Effective configuration, origin, and `restart_required` |
| GET | `/api/v1/config/env` | `KEY=value` form |
| PUT | `/api/v1/config` | Patch the file layer. Hidden when `WEB_ENABLE=false` |
| GET | `/api/v1/config/export` | One JSON document of the effective configuration. No secrets exist to omit |
| POST | `/api/v1/config/import` | Restore that document into the file layer. Environment variables still win |

This agent has no IS-05 senders or receivers. It does not accept connection patches. It only reads other nodes' active connections.

## Running on the platform

The platform runs one agent per node as a DaemonSet on the host network. Host networking is required for RDMA and for the fabric addresses in the peer map. `deploy/mxl-fabrics-agent.yaml` shows the contract: no `hostIPC`, `IPC_LOCK` for memory registration, the MXL root hostPath at `/Volumes/mxl`, a writable `/config` volume, probes on `/livez` and `/readyz`, and `terminationGracePeriodSeconds` greater than `SHUTDOWN_TIMEOUT_S`.

Set `NMOS_HOST_ADDRESS` to the node IP, `NMOS_DNS_SD=false`, and `NMOS_REGISTRY_PORT` to the platform registration port (the query port defaults to that port plus 1). Set `LOCAL_NODE_CIDRS` to the node's pod CIDR. Set `MXL_CLEANUP_ON_EXIT=true` so this node's mirrors disappear on shutdown, or `false` so readers on the node keep their mirror flows across an agent restart. Leave `NMOS_SEED` unset to keep the existing node id, which is UUIDv5 of `HOST_ID`. Setting `NMOS_SEED` changes that id.
