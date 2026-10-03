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

1. Receivers resolve `mxl_domain_id` by scanning domains under the MXL root. Mirror domains are siblings of local domains (`mirror-<source-domain-id>`). The id is the `id` inside `domain_def.json`, not the directory name.
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

It checks that the eager mirror exists before the receiver is activated, that grain indices match, that disabling the receiver releases the target after `RELEASE_GRACE_MS`, that stopping the source is reported as peer down, that replication resumes, and that a flow id the source no longer has is `stale_reference`.

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

`deploy/mxl-fabrics-agent.yaml` is a DaemonSet: `hostNetwork`, `IPC_LOCK`, the MXL root hostPath, and a ConfigMap selected by `HOST_ID` (downward API node name). Set `LOCAL_NODE_CIDRS` in that per-node file to the node's pod CIDR (`kubectl get node <name> -o jsonpath='{.spec.podCIDR}'`) so NMOS Nodes on the pod network of the same machine count as local. RDMA device access reuses the generic device plugin pattern from mxl-decklink, exposing `/dev/infiniband`. `deploy/monitoring/` has a ServiceMonitor example. The Grafana dashboard is `deploy/grafana/mxl-fabrics-agent.json`.

## Configuration

Environment variables override `AGENT_CONFIG_FILE`, which overrides the defaults. Invalid configuration exits 78. Peer entries and the mirror include/exclude lists apply at runtime. Other keys set `restart_required` in the UI.

| Key | Default |
| --- | --- |
| `HOST_ID` | hostname |
| `MXL_ROOT` | `/Volumes/mxl` |
| `MIRROR_MODE` | `eager` (`eager` or `on-demand`) |
| `MIRROR_INCLUDE_DOMAINS` / `MIRROR_INCLUDE_FLOWS` | empty (no extra filter) |
| `MIRROR_EXCLUDE_DOMAINS` / `MIRROR_EXCLUDE_FLOWS` | empty |
| `DEFAULT_PROVIDER` | `verbs` |
| `PROVIDER_FALLBACK` | empty, or `tcp` |
| `WEB_PORT` | 8095 |
| `NMOS_PORT` | 3232 |
| `LOCAL_NODE_CIDRS` | empty (this node's pod CIDR when media functions use the pod network) |

`WEB_ENABLE=false` removes the UI and config writes. `/api/v1` stays available for peers.
