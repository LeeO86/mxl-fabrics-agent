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
5. Readers on a destination should sit at least one grain behind the mirror head (`mxl_fabrics_agent_replication_lag_ms`; `…_lag_grains` counts transfer batches for audio, which can be single samples).
6. A writer may leave indexes unwritten (one that is late and jumps to the current grain). The source passes over them, and they are invalid on the mirror as on the origin. `mxl_fabrics_agent_replication_origin_gaps_total` counts them.

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

The agent watches each peer's link: the carrier of the local interface that holds `local_fabric_addr` (or `FABRIC_INTERFACE`), and that some interface holds the address at all. A dead link is `link_down` with the reason in `/api/v1/peers` and `/api/v1/replications`, `mxl_fabrics_agent_peer_up{peer,interface}` is 0, and the log has one `peer_link_down` line. The destination then drops its targets for that peer and retries nothing; when the link is back (`peer_link_up`) it sets up fresh ones. libfabric's `verbs` provider keeps the address list of the process's first `fi_getinfo`, so an address that was missing when the agent started stayed unknown after it came back, until a restart ("provider verbs has no interface for …", the peer saw `http 500`); since 1.3.1 the agent builds that list again when an interface holds the address with carrier (`fabric_interfaces_rescanned`). `mxl_fabrics_agent_peer_rdma_retransmits_total{peer,device}` is the RDMA device's `RetransSegs` (irdma): a lossy link shows there first. Flows are not relayed over a third host.

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
sudo env LD_LIBRARY_PATH=/opt/mxl/lib:/usr/local/lib FI_PROVIDER=tcp tests/integration/flow_cleanup.sh
```

It checks that the eager mirror exists before the receiver is activated, that grain indices match, that disabling the receiver releases the target after `RELEASE_GRACE_MS`, that stopping the source is reported as peer down, that replication resumes, that a flow id the source no longer has is `stale_reference`, that a 1080p50 flow with a 200 ms ring arrives at 50 grains/s, that a 2-channel 48 kHz flow written in 1 ms batches arrives at 48,000 samples/s, that a flow its writer creates again (new inode) replicates again, that a destination that goes away costs the source a few log lines, not one per pass, and that a writer that leaves indexes unwritten replicates every written grain with the gaps counted. `flow_cleanup.sh` ends two of three origin flows in one domain and checks the dry run, the removal of the flow nobody reads, that a mapped flow stays until its reader exits, and that the live flow and the domain stay (root, or the reader check is incomplete and it checks only the refusal).

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
| `MIRROR_IDLE_GRACE_S` | 300 | Drop a mirror domain after this many seconds without demand, and a mirror flow after its origin flow has been gone this long (`0`: never) |
| `MIRROR_CLEANUP_DRY_RUN` | false | Mirror cleanup only logs `mirror_domain_would_remove` and `mirror_flow_would_remove` |
| `MIRROR_STALE_MS` | 2000 | A mirror flow without a new grain for this long while grains are due is `stale` (see Mirror health). `0`: never |
| `MIRROR_ADMIN_REBUILD` | false | Allow `POST /api/v1/mirrors/{domain_id}/{flow_id}/rebuild`, the test hook that rebuilds one mirror flow (see Mirror health). Environment only, read at start: the config file, `PUT /api/v1/config` and the import cannot set it, and the export does not show it |
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
| GET | `/api/v1/inventory` | Local domains and flows, with each flow's `head_index` at the time of the request |
| GET | `/api/v1/mirrors` | Mirror domains: origin, flows (with `health`: stale, rebuilt), demand, readers, age, cleanup state |
| DELETE | `/api/v1/mirrors/{domain_id}` | Remove a mirror domain; 409 while another process maps it |
| POST | `/api/v1/mirrors/{domain_id}/{flow_id}/rebuild` | Test hook: rebuild one mirror flow so its readers get `MXL_ERR_FLOW_INVALID`; 403 unless `MIRROR_ADMIN_REBUILD=true` |
| GET | `/api/v1/demand` | Local IS-05 demand |
| GET | `/api/v1/replications` | Active replications: state, `grains`, `head_index`, `lag_grains` (transfer batches for continuous flows), `lag_ms` (samples ÷ sample rate, or grains × grain period), `origin_gaps`, errors |
| POST | `/api/v1/replications` | Peer asks this host to send a flow |
| DELETE | `/api/v1/replications/{id}/targets/{dest}` | Peer releases a target |
| GET | `/api/v1/peers` | Peer status: control plane `up`, fabric `link_up`, `link_error`, `fabric_interface` |
| POST | `/api/v1/peers/{id}/test` | Control-plane and local target check |
| GET | `/api/v1/events` | Server-sent events: `inventory`, `replication`, `peer`, `mirror` |
| GET | `/api/v1/config` | Effective configuration, origin, and `restart_required` |
| GET | `/api/v1/config/env` | `KEY=value` form |
| PUT | `/api/v1/config` | Patch the file layer. Hidden when `WEB_ENABLE=false` |
| GET | `/api/v1/config/export` | One JSON document of the effective configuration. No secrets exist to omit |
| POST | `/api/v1/config/import` | Restore that document into the file layer. Environment variables still win |

## Mirror cleanup

The agent drops whole destination mirror domains (`<MXL root>/mirror-<domain id>`): it closes its own writers and targets of the domain, removes its flows, then the directory. Each tick gives every mirror domain on disk one state:

| State | When | What happens |
| --- | --- | --- |
| `same_node` | The origin domain is on this node (a function moved here) | Removed at once. The agent never creates a mirror of a local origin. Readers must find the origin, not a frozen copy with the same flow ids |
| `orphan` | No peer that is up lists the origin domain, and its source peer is not a known peer that is down | Removed at once |
| `active` | A receiver wants one of its flows, a replication is held, or `MIRROR_MODE=eager` mirrors a live origin | Kept |
| `idle` | None of that | Removed after `MIRROR_IDLE_GRACE_S` without demand |

Safety:

- Only directories named `mirror-<domain id>` whose `domain_def.json` carries this agent's mirror marker. Origin domains, other agents' mirrors and anything else under the root are never touched.
- Nothing is removed while a process other than the agent maps one of its files. The agent reads `/proc/<pid>/maps` of every process and matches device and inode, so the mount namespace of the reader does not matter. MXL readers take no lock, so the maps are the only sign of a reader. When the agent cannot see every process (its own PID namespace, or a `maps` file it may not read), it removes nothing and says so (`readers_known: false`, `readers_error`). On the platform the agent therefore needs `hostPID: true` and `SYS_PTRACE` (the image raises it from its file capabilities, like `SYS_NICE`); the runtime's default AppArmor profile denies `ptrace read` of processes outside it, so the agent also needs `appArmorProfile: Unconfined` (lab: with docker's default profile every mirror stayed, `readers_known: false`).
- The check runs again after the agent closed its own writers, right before the files go.
- Orphans are decided only 30 s after start and, with NMOS, while the registry answers: a registry outage does not make every mirror an orphan. Nothing is removed in the first 30 s.
- `MIRROR_CLEANUP_DRY_RUN=true` logs `mirror_domain_would_remove` and removes nothing (also for `DELETE`).

Logs: `mirror_domain_removed` (domain, path, reason, flows), `mirror_domain_busy` (refused, with the reader pids), `mirror_domain_would_remove` (dry run). Metrics: `mxl_fabrics_agent_mirror_domains{state}`, `mxl_fabrics_agent_mirror_domains_removed_total{reason}` and `mxl_fabrics_agent_mirror_domains_refused_total{reason}`; `reason` is `same_node`, `orphan`, `idle` or `api`.

### Single mirror flows (1.6.0)

A mirror domain stays while one of its flows is in use, but a flow whose origin flow is gone (the function deleted it, or a new flow id replaced it) used to stay with it for good: health `idle`, reason `origin_missing`, its last grain days old. The agent now removes such a flow (`<flow id>.mxl-flow` in the mirror domain) on its own:

- Its origin is missing: the domain's source peer is up and no up peer offers the flow. A source peer that is down may still hold it, so its flows wait, as whole domains do.
- It has been missing for `MIRROR_IDLE_GRACE_S` (the domain setting, default 300 s, `0` never). The clock starts again when the flow is offered again or its source goes down, and after an agent restart.
- Nothing goes in the first 30 s after start or, with NMOS, while the registry does not answer.
- Nothing counts as missing until the inventories that decide are settled (1.7.1). That is the domain's source peer and any up peer that lists the domain: the peer has been up for one full poll (two polls in a row with the same boot id) and its inventory comes from a finished scan. Meanwhile the health reason is `inventory_pending`, and a running grace clock pauses. A peer that stays unsettled for over 60 s counts as down for this. An agent answers `/api/v1/inventory` with 503 until its first scan.
- The reader check is the one for whole domains, per flow: the files of that flow directory, matched by device and inode in `/proc/<pid>/maps` of every other process. A flow that another process maps stays (`mirror_flow_kept_reader` with the pids), the domain's other flows do not matter. It runs again after the agent released its own writer of the flow, right before the files go. When the agent cannot see every process, nothing is removed.
- Same ownership rules: only flows in `mirror-<domain id>` directories with this agent's marker. Origin domains and other agents' mirrors are never touched.
- `MIRROR_CLEANUP_DRY_RUN=true` logs `mirror_flow_would_remove` and removes nothing.

The removed flow is `absent` in the mirror health with reason `origin_missing` and leaves `mirror_state`; should the origin come back, the new mirror counts as a rebuild with that reason. Logs: `mirror_flow_origin_missing` (once, when the grace time starts), `mirror_flow_removed` (`domain_id`, `flow_id`, `path`, `reason`, `missing_s`), `mirror_flow_kept_reader` (`refused`: the reader pids or why the check is incomplete; once per change), `mirror_flow_would_remove`. Metrics: `mxl_fabrics_agent_mirror_flows_removed_total{reason}` (`origin_missing`; also the per-flow removals of an offered flow: `origin_gone`, `no_demand`, `excluded`) and `mxl_fabrics_agent_mirror_flows_refused_total{reason}`.

`GET /api/v1/mirrors` lists one entry per mirror domain: `domain_id`, `path`, `origin` (`host_id`, `peer_up`, `on_peer`, `local`), `state`, `on_disk`, `demand`, `idle_since` (Unix seconds, `null` with demand), `age_s` (since its `domain_def.json` was written), `readers` (`pid`, `comm`), `readers_known`, and `flows` (`flow_id`, `media_type`, `format`, `active`, the per-flow mirror `state`). `DELETE /api/v1/mirrors/{domain_id}` removes one with the same checks: 404 when this agent has no such mirror, 409 with `readers` or `error` while it is mapped or the readers are unknown, 200 with `removed: true` otherwise. A mirror that is still wanted (`eager`, demand) comes back on the next tick.

This agent has no IS-05 senders or receivers. It does not accept connection patches. It only reads other nodes' active connections.

## Audio timing

How a continuous (audio) flow moves: the source's lane (its own thread, a pass every 1 ms) reads the origin's head and posts one transfer with every sample written since the last one, at most one bounce-buffer entry (the mirror's sync batch, 10 ms by default). One transfer is in flight at a time; the next one starts 2 ms after the source saw the last one complete (over `verbs` the target posts the receive for the next immediate only when it drains the last transfer). The destination's lane drains its target and commits each batch to the mirror. Nothing waits for a grain or a sample count: with the ST 2110 gateway's 1 ms commits a mirror gets a batch of about 3 ms every 3 ms (lab, `tcp`; 4 ms with 1.7.1). After a stall the source sends one entry per transfer until it has caught up, so a 10 ms entry and a 7 ms transfer period catch up only 3 ms per transfer.

`lag_ms` (and `lag_grains`) is computed on each pass of the destination's lane from the mirror's head and the current TAI index, and `/api/v1/replications` and the gauge show the value at the moment they are read. It is 0 when the head is at or ahead of the current index (an origin that writes ahead) and while the origin is not written. A 1 s scrape therefore misses a stall of a few hundred milliseconds. For bursts use (since 1.7.2):

- `replication_sample_age_seconds{stage}` and `replication_sample_age_max_seconds{stage}` (largest of the last 10 s, the same for every scraper): how old the samples are when the source reads the origin (`origin`), posts them (`sent`) and the destination commits them (`delivered`). A reader with a 120 ms window misses samples whose `delivered` age is above 0.12.
- `replication_delivery_gap_seconds` and `replication_late_deliveries_total` at the destination: the time between two commits, and the gaps longer than twice the flow's usual gap (a slow moving average) and at least 10 ms. A gap includes the origin's own pauses: a writer that commits nothing for 50 ms gives a 50 ms gap.
- `sample_transfer_seconds` and `sample_transfer_max_seconds` (last 10 s) at the source: one transfer from its post to its completion. A long one with `fabric_thread_stall_seconds` flat is the transport (a retransmission or receiver-not-ready wait), compare `peer_rdma_retransmits_total`.
- `fabric_thread_stall_seconds{task}` and the log `fabric_thread_stall` (`gap_ms`, `task`, `task_ms`, at most one line per second and lane): a lane's thread made no pass for over 20 ms while it had work, and the call that held it (`pass`: none, the thread got no CPU).
- `reconcile_work_seconds{work}`: the periodic work (`plan`, `pulls`, `cleanup_domains`, `cleanup_flows`, `health`, `lanes`, `metrics`, `tick`, `reader_scan`, `peer_poll`, `inventory_scan`). `LOG_LEVEL=debug` logs each (`work_timing`, `peer_poll_timing`); one over 0.5 s logs `reconcile_work_slow` (info, at most every 10 s). None of this work runs on a lane's thread.

The lanes count their timing themselves and the reconcile pass copies it into the metrics every 200 ms, so a scrape never holds up a lane. A lane's counts start again when the lane does (its flow had nothing left), which Prometheus treats as a counter reset; `grain_transfer_seconds` is counted the same way since 1.7.2.

**Lanes and inotify.** Each flow has its own thread and Fabrics instance; the flows of one domain share one MXL instance, as before. MXL opens one inotify instance per MXL instance, and `fs.inotify.max_user_instances` (128 by default) counts them per user on the whole host, together with the media functions that run as the same user. When MXL cannot open one, the lane logs `domain_fabric_failed` with a hint, and the flow is not replicated: raise the limit (e.g. `sysctl fs.inotify.max_user_instances=1024`) on hosts with many domains.

**`RT_PRIORITY` and `CPU_AFFINITY`.** Every lane's thread gets the same SCHED_FIFO priority and CPU set. A narrow `CPU_AFFINITY` (one core) puts all lanes on that core, where a lane waits for the passes of the others; it does not isolate them. Give the lanes as many cores as the platform allows, or leave `CPU_AFFINITY` empty.

## Mirror health

The agent says when a destination mirror flow no longer carries its source. It does not re-route: choosing a receiver's source is the controller's job over IS-05.

Each reconcile pass (about every 200 ms) gives every mirror flow one state:

| State | When |
| --- | --- |
| `live` | Grains are due and the newest one is younger than `MIRROR_STALE_MS` |
| `pending` | Grains are due, but for less than `MIRROR_STALE_MS` so far (a replication that just started, or a rebuilt flow) |
| `stale` | Grains are due, none came for `MIRROR_STALE_MS`, and the origin has newer ones (or the source peer or link is down) |
| `idle` | No grains are due: no receiver wants the flow, or the origin is not written |
| `absent` | The flow is not on disk |

"Grains are due" means a replication of the flow is wanted (a receiver on this host, or a held replication) and the origin is live in the source's inventory. While the source peer or its fabric link is down they stay due as long as the receiver does. The age of the newest grain is the mirror's head index against the current TAI index at the flow's rate, so it includes the replication lag. A source's `/api/v1/inventory` now carries each flow's `head_index`; the destination uses it to tell a mirror that misses grains (`stale`) from an origin that writes none (`idle`, `origin_silent`: a writer without input signal). With an older source the origin's `live` flag stands in, so a source that stops writing can show `stale` for up to about 5 s plus `PEER_POLL_INTERVAL_MS`. Detection takes up to `MIRROR_STALE_MS` or one peer poll, whichever is longer.

`reason`: `no_new_grains`, `no_first_grain`, `peer_down`, `link_down` (stale); `no_demand`, `origin_inactive`, `origin_not_live`, `origin_missing`, `inventory_pending`, `origin_silent`, `peer_down` (idle); the removal reason (absent), e.g. `origin_missing` after the single-flow cleanup.

A mirror is **rebuilt** when its data file is a new one: another inode, or the file back after it was gone. MXL readers keep the file they opened, so a reader of the old one sees no new grain again (the MXL inode quirk). The rebuild reason is why the agent removed it: `origin_gone` (origin inactive longer than `MIRROR_GRACE_S`), `origin_missing` (origin flow gone for `MIRROR_IDLE_GRACE_S`), `no_demand`, `excluded`, `mirror_domain_idle`, `mirror_domain_orphan`, `mirror_domain_same_node`, `mirror_domain_api`, `origin_config_changed` (the origin came back with another flow definition), `api` (the test hook), or `replaced` when something else removed or replaced it. Counts and times are kept in memory for 24 h after a mirror was last seen; an agent restart starts them again. Link loss and an origin writer that re-creates its flow with the same definition do not rebuild the mirror: the destination keeps its target and mirror flow, which then goes `stale` and back to `live`.

`GET /api/v1/mirrors` has `health` on each flow: `state`, `reason`, `last_grain_age_ms` (`null` before the first grain), `stale_since` (Unix seconds or `null`), `stale_after_ms`, `rebuilds`, `last_rebuild` (Unix seconds or `null`), `last_rebuild_reason`.

Metrics (labels `domain_id`, `flow_id`, `peer` = source host):

- `mxl_fabrics_agent_mirror_stale` gauge, 1 while `stale`, else 0
- `mxl_fabrics_agent_mirror_state` gauge, 1 for the current `state` label (`live`, `pending`, `stale`, `idle`)
- `mxl_fabrics_agent_mirror_last_grain_age_seconds` gauge, absent before the first grain
- `mxl_fabrics_agent_mirror_rebuilds_total` counter
- `mxl_fabrics_agent_mirrors_stale` gauge, no labels: mirror flows that are `stale`

With `METRICS_PER_FLOW=false` only `mirrors_stale` and an unlabelled `mirror_rebuilds_total` remain. Absent mirrors have no `mirror_stale`, `mirror_state` or age series.

Logs: `mirror_stale` (warn: `domain_id`, `flow_id`, `peer`, `reason`, `last_grain_age_ms`, `stale_after_ms`), `mirror_recovered` (info: the new `state` and `reason`, `stale_ms`; `state=live` is a real recovery), `mirror_rebuilt` (warn: `reason`, `rebuilds`, `old_inode`, `inode`). The same three go out as the SSE event `mirror` with `event` = `stale`, `recovered` or `rebuilt` and the `health` fields.

### Rebuild rules

An origin writer that restarts does not by itself rebuild the mirror. What counts is what the destination sees of the origin in the source's inventory (polled every `PEER_POLL_INTERVAL_MS`, sooner on its SSE events) while it is away:

| The destination sees | Mirror | `mirror_rebuilt` reason when the origin is back |
| --- | --- | --- |
| The origin back with the same flow definition before a rule below fires, whatever the gap | Kept: grains go on in the same file after a stall (`stale`, then `live`) | none |
| The origin listed but inactive (writer killed, its files left behind), with a receiver on this host | Kept (`orphaned` after `MIRROR_GRACE_S`) | none |
| The same without a receiver on this host, longer than `MIRROR_GRACE_S` (10 s) | Removed, without a reader check | `origin_gone` |
| The origin flow gone (MXL deletes it with its last writer on a clean exit), its domain still listed, for `MIRROR_IDLE_GRACE_S` (300 s), and no process maps the mirror flow | Removed | `origin_missing` |
| The origin domain gone (the function deletes its domain on exit, e.g. mxl-replay), seen on one peer poll, and no process maps a file of the mirror domain at the next reader check (every 2 s) | Removed at once | `mirror_domain_orphan` |
| The origin back with another flow definition (rate, format, media type, geometry; not label, description, tags, parents, version, source or device id) | Rebuilt at once, readers or not: the old layout is wrong for them too | `origin_config_changed` |
| Source peer or link down; this agent restarted with `MXL_CLEANUP_ON_EXIT=false` | Kept | none |
| `MIRROR_MODE=on-demand` and no demand for `RELEASE_GRACE_MS` | Removed | `no_demand` |
| `POST /api/v1/mirrors/{domain_id}/{flow_id}/rebuild` | Rebuilt at once | `api` |

Given the timing the outcome is deterministic, but a restart that deletes the whole origin domain races the peer poll and the reader check: a gap shorter than a poll can go unseen, and a mirror that a reader maps at the check stays. A writer killed and started again over its old files re-opens them: the origin keeps its file and its old definition, so nothing changes on the mirror either.

### Test hook: rebuild one mirror

`POST /api/v1/mirrors/{domain_id}/{flow_id}/rebuild` makes one mirror flow on this host look like an origin that came back with a new definition, to test a media function's re-open path without restarting a producer. It drops the flow's target and the source's connection to it, releases the agent's writer, removes the flow's files and creates the flow again from its own `flow_def.json`, under the same ids. The next reconcile pass (about 200 ms) sets up a new target and replication resumes into the new file. Readers that hold the old file get `MXL_ERR_FLOW_INVALID` once no new grain comes, and have to open the flow again (IS-05 receivers: activate them again, as for any rebuild).

- Off by default. Start the agent with `MIRROR_ADMIN_REBUILD=true` in its environment to allow it. Only the deployment can open the hook: the setting is environment-only and read at start, a config file that holds it is ignored, `PUT /api/v1/config` with it answers `400`, the import skips it and the export does not show it. Like the rest of `/api/v1` the hook has no authentication (protected networks only).
- Answers: `403` while it is off, `400` for ids that are not UUIDs, `404` when this agent has no such mirror flow, `200` with `old_inode`, `inode` and the `readers` (`pid`, `comm`) that mapped the old file, `500` when the flow could not be created again (the next pass retries while the mirror is wanted).
- Logs `mirror_rebuild_requested` at once and `mirror_rebuilt` with reason `api` on the next pass; `mirror_rebuilds_total` rises and the SSE event `mirror` goes out like for any rebuild. `MIRROR_CLEANUP_DRY_RUN` does not apply.

```bash
curl -X POST http://<node>:8095/api/v1/mirrors/<domain_id>/<flow_id>/rebuild
```

### Platform contract

Watch `mxl_fabrics_agent_mirror_stale` and `increase(mxl_fabrics_agent_mirror_rebuilds_total[…])` per `domain_id`/`flow_id` on each destination host, or subscribe to the `mirror` SSE event. A receiver on that host that reads a `stale` mirror gets no new grains: after the platform's own hold-off (a few seconds absorbs a replication restart) it may route the receiver to another source over IS-05. When the rebuild counter rises, every receiver on that host that reads the flow should be activated again over IS-05, also with the same sender, so the media function opens the new file. `idle` with `origin_silent` is a source without signal, not a mirror fault: re-routing to another path of the same source does not help. `stale` with `peer_down` or `link_down` means the path to the source is gone. The agent itself never PATCHes IS-05.

## Running on the platform

The platform runs one agent per node as a DaemonSet on the host network. Host networking is required for RDMA and for the fabric addresses in the peer map. `deploy/mxl-fabrics-agent.yaml` shows the contract: no `hostIPC`, `hostPID`, `SYS_PTRACE` and an unconfined AppArmor profile for the mirror reader check, `IPC_LOCK` for memory registration, the MXL root hostPath at `/Volumes/mxl`, a writable `/config` volume, probes on `/livez` and `/readyz`, and `terminationGracePeriodSeconds` greater than `SHUTDOWN_TIMEOUT_S`.

Set `NMOS_HOST_ADDRESS` to the node IP, `NMOS_DNS_SD=false`, and `NMOS_REGISTRY_PORT` to the platform registration port (the query port defaults to that port plus 1). Set `LOCAL_NODE_CIDRS` to the node's pod CIDR. Set `MXL_CLEANUP_ON_EXIT=true` so this node's mirrors disappear on shutdown, or `false` so readers on the node keep their mirror flows across an agent restart. Leave `NMOS_SEED` unset to keep the existing node id, which is UUIDv5 of `HOST_ID`. Setting `NMOS_SEED` changes that id.
