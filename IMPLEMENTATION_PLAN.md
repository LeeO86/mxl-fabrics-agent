# mxl-fabrics-agent — Implementation Plan

This document records how `SPECIFICATION.md` (draft v0.1) is implemented, including
places where the real MXL 1.1 Fabrics API differs from the spec's paraphrase.

## 1. Pins

| Component | Pin |
| --- | --- |
| MXL | `dmf-mxl/mxl` `release/v1.1` at `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7` (includes the `validSlices` reset, issue 717) |
| libfabric | `v2.3.1` in the image (`FI_SOCKADDR_IP` / tcp). Any libfabric ≥ 2.3 with the `tcp` and `verbs` providers is acceptable |
| nmos-cpp | `fe303849527394b03bdedc8f161f377fe458bb62` (same commit as mxl-decklink) |
| Provider hardware | `verbs` via libibverbs. Intel E810 uses the `irdma` kernel provider. NVIDIA/Mellanox ConnectX uses `mlx5`. Both are the same Fabrics provider (`verbs`) |

The endpoint-id collision fix and the configurable target completion-queue depth (`cqDepth`) are in this MXL pin. The separate batch call `mxlFabricsTargetReadGrainsNonBlocking` was not merged. Targets drain with a tight loop of `mxlFabricsTargetReadGrainNonBlocking`, then commit.

## 2. Deviations

1. **One fabrics instance per MXL domain, not per provider.** `mxlCreateInstance` takes one domain directory and `mxlFabricsCreateInstance` wraps that instance. Provider (`verbs` or `tcp`) is chosen per target or initiator. Calls for one domain run on that domain's thread.
2. **One initiator per (origin flow, local fabric address).** A single initiator binds one address. On a switched network that is one initiator per flow. On a direct-link mesh, a flow sent to two peers on two NICs uses two readers and two initiators.
3. **Include and exclude filters are separate.** `MIRROR_INCLUDE_DOMAINS`, `MIRROR_INCLUDE_FLOWS`, `MIRROR_EXCLUDE_DOMAINS`, `MIRROR_EXCLUDE_FLOWS`. An empty include list does not filter. The combined `MIRROR_INCLUDE` / `MIRROR_EXCLUDE` keys are rejected because domain ids and flow ids are both UUIDs.
4. **`/api/v1/info` includes `boot_id`.** A new UUID per process. Destinations repeat the handshake when it changes or when the peer inventory revision goes backwards.
5. **Repeated `POST /replications` replaces `target_info`** for the same `(flow_id, dest_host_id)` and returns the existing `replication_id`.
6. **`WEB_ENABLE=false` hides the UI and config writes.** `/api/v1` stays up so peers still work. Health and metrics stay up.
7. **`METRICS_PER_FLOW` defaults to true.**
8. **Grain commit snapshots metadata first.** On this MXL revision `mxlFlowWriterOpenGrain` clears `validSlices` and the invalid flag. The agent reads `mxlFlowWriterGetGrainInfo` before opening and commits that snapshot, so the index, flags, and slice count written by Fabrics are preserved.
9. **`on-demand` is implemented.** mxl-decklink and mxl-st2110-gateway reject an IS-05 activation whose domain is not already on disk, so those receivers need `MIRROR_MODE=eager` (the default). `on-demand` is for readers that retry domain discovery.
10. **Query observation is polled** every `NMOS_POLL_INTERVAL_MS` (and on the reconcile loop). Correctness does not depend on a single websocket event. The agent's own Node is a real nmos-cpp node and still serves its HTTP and websocket listeners.
11. **Test connect** (`POST /api/v1/peers/{id}/test`) checks the peer control plane and sets up a local fabrics target on the configured provider and address. It removes that target before returning.
12. **Metrics (spec §12) are partly implemented.** `replication_errors_total` has no `kind` label: the fabric pass counts errors and keeps the last message, it does not classify them. `setup_seconds` and `nmos_poll_errors_total` are not implemented (1.0.3); `grain_transfer_seconds` is since 1.1.0.
13. **Dead links are detected by missing grains, not by a fabric error.** A closed `tcp` connection or a hung peer often reports nothing to the initiator or the target. MXL's RC initiator (at the pin) only logs a completion error: the failed transfer stays pending, `mxlFabricsInitiatorMakeProgressNonBlocking` returns `NOT_READY` forever and the agent sends nothing more. The destination rebuilds its target when no grain arrived for 5 s while the link is in `error` or the peer inventory shows the source flow being written; the wait doubles per rebuild in a row up to 40 s. The source marks a link connected but without a completed transfer for 2 s as `error`. One initiator serves all destinations of a flow, so a dead destination blocks the others until it is rebuilt (1.0.3).
14. **Lag is measured against TAI, not the source's reported head.** The destination never sees the origin head; origin writers commit at the current TAI index, so the lag is `mxlGetCurrentIndex(rate) − mirror head` while a writer holds the origin flow (1.0.3).
15. **Mirror writers are not released on SIGTERM without cleanup.** MXL deletes a flow when its last writer is released (`Instance::releaseWriter`) or its instance destroyed. With `MXL_CLEANUP_ON_EXIT=false` the agent leaves writers and instance to the process exit, so the flows and their inodes survive and readers keep them (1.0.3).
16. **Transfer pacing is an agent setting, not an MXL change (1.1.0, user decision 2026-10-06).** The platform's small cluster has Intel E810-XXV cards at PCIe Gen3 x8 (the CPUs have Gen3 only): about 6.5 GB/s of DMA for two 25G ports. Grains sent as one 5.5 MB burst at line rate on both ports at once overrun the receiving RDMA engine although PFC is in effect (`ip4InDiscards` on the receiver equal `RetransSegs` on the sender), and now and then a queue pair runs out of retries. ice offers no rate limit for RDMA queue pairs (no `dcb maxrate`, no devlink rates, mqprio shapes LAN queues only). `TRANSFER_PACING=frame` (default `off`) sends a complete grain in `TRANSFER_PACING_BATCHES` slice batches whose starts are spread over `TRANSFER_PACING_SPREAD` of the grain duration, through MXL's public slice-range transfer (`mxlFabricsInitiatorTransferGrain(index, start, end)`); MXL stays upstream. The destination takes the grain index from the header in the first batch, so the source starts a grain again from slice 0 after a target change or a pause of more than 100 ms. The fabric thread sleeps until the next batch is due instead of 2 ms. Options considered: a separate image (a second artifact for the same code) and a NIC rate cap (not available). The platform turns it on for the small cluster only.

## 3. Process

One thread reconciles scanner, peer, and NMOS snapshots into mirror plans (`src/mirror/lifecycle.cpp`) and pull requests. Each MXL domain that the agent opens has a fabric thread that serialises MXL and Fabrics calls, drains target completion queues, and transfers new grains or samples. Peer inventory polling and the NMOS observer run on their own threads. The HTTP server is another thread.

`verbs` setup failure with `PROVIDER_FALLBACK=tcp` retries that link on `tcp` and reports `fallback: true`.

## 4. Local nodes on the pod network

`LOCAL_NODE_CIDRS` (empty by default, not a runtime key) adds a fourth local-node
rule after the host-address, hostname, and static-list rules, and after the
agent's own Node is excluded. An endpoint host that is an IP literal is matched
directly. A DNS name is resolved with `getaddrinfo` only when the list is
non-empty, with a 250 ms bound and a 30 s positive and negative cache. A name
that does not resolve is not local and is logged once at debug. The match rule
(`list`, `hostname`, `ip`, `cidr`) is logged at debug as `nmos_node_local`.

The platform renders each node's pod CIDR into that agent's config. The agent
does not watch the Kubernetes API or derive the CIDR from routes. A later
`LOCAL_NODE_CIDRS=auto` could do that.

## 5. Tests

- Unit tests cover config precedence, local-node matching (including `LOCAL_NODE_CIDRS`), IS-05 demand derivation, the mirror state machine, include/exclude filters, inventory classification, and handshake idempotency.
- `tests/integration/tcp_mesh.sh` runs two agents on one machine with the `tcp` provider: eager mirror before activation, identical grain indices, release after grace, peer down, resume, and `stale_reference`.
- Hardware checks in spec §15.4 (sustained `verbs` on E810 and ConnectX) are not run in CI.
- Lab run 2026-10-03, one host (2× Xeon Gold 6136, no RDMA device, so `tcp` over loopback only), two agents (1.0.0 and this tree), lab stand-in NMOS with 16 receivers, 1080p50 v210 sources from mxl-test-player (1 s history) and `mxl-mv-writer` (MXL default history, 10 grains):
  - One 1 s-history flow: 49.5 grains/s, `lag_grains` 0, 0.13 cores per agent. Mirrored pixels equal the origin.
  - 16 concurrent 1 s-history flows: 30 grains/s on average, about 2.7 GB/s in total, about 1.2 cores per agent. The `tcp` provider on this host carries about ten 1080p50 flows; the rest need `verbs`.
  - A flow with a 10-grain ring (200 ms at 50p) replicates at about 25 grains/s even alone. The initiator gets `MXL_ERR_NOT_READY` from `mxlFabricsInitiatorMakeProgressNonBlocking` on about every other pump (state `pending`), its reader falls out of the 10-grain ring (`TOO_LATE`) and resyncs; the destination logs `open grain 8` (`MXL_ERR_INVALID_ARG`, index not after the last commit). Open: check with `verbs`; until then use at least 1 s history for flows that cross hosts over `tcp`.
  - Cause (found in CI, 2026-10-04): the destination repeats its `POST /replications` on every reconcile pass and the source removed and re-added the same target each time, so the initiator reconnected every pass. Fixed in 1.0.1 (an unchanged target is kept); the `tcp_mesh.sh` check replicates a 200 ms-ring 1080p50 flow at 50.3 grains/s with none missed. The 1 s-history advice above is no longer needed.
  - Eager mode mirrored every flow of both lab domains (4.7 GB of tmpfs for 16 video flows with 1 s history plus audio and data). With Docker's default open-file limit the mirrors failed after about 30 flows; fixed in this tree.
  - `/api/v1/replications` shows `state: pending` for links that move 50 grains/s, and `mxl_fabrics_agent_replication_lag_grains` stayed 0 while a link lost half its grains. The `pending` label is fixed in 1.0.2 (see the 2026-10-06 run).
  - Not covered: `verbs`, the 3-host mesh, the 24 h run, fan-out over two links, completion queues under RDMA load.
- Lab run 2026-10-06, same host and setup, `verbs` over soft-RoCE (`rdma_rxe` on `eno3`, `/dev/infiniband/uverbs0` and `rdma_cm` mapped into both agents):
  - 1.0.1 has no `/etc/libibverbs.d` and no `ibv_devices`; libibverbs finds no device. With `ibverbs-providers` (1.0.2) `ibv_devices` in the container lists `rxe0`.
  - 4 flows (1 s history): 50.2 grains/s, lag 0, no errors, no fallback, about 0.5 cores on the source agent.
  - 8 flows (four with 1 s history, four with MXL's 200 ms default): about 40 grains/s per flow, the same with 1.0.1 code. Soft-RoCE does the RDMA work, including a CRC per packet, on the CPU of one host; 8 flows are about 2.2 GB/s. The destination reports `open grain 8` (`MXL_ERR_INVALID_ARG`): the source falls out of its ring and jumps ahead, the destination writer then invalidates the skipped slots by rewriting their headers, and grains that had already arrived in those slots are read with the old index. More on 200 ms-ring flows (about 20 per flow in 45 s) than on 1 s flows (2–5). Real NICs are not limited like this; spec §15.4 on E810 and ConnectX is still open.
  - `tcp` on the same host, 8 flows: 50.2 grains/s, no errors. 1.0.1 showed 4 of 8 links `pending`; 1.0.2 shows all `active`.
- Lab run 2026-10-06 (1.0.3), same host, two agents over `tcp`, 4 receivers routed to node-a's flows (the platform had reported a destination that stayed `error` with `last_error` `recv` and `restarts` 0, and `last_error` left on links that work):
  - Hung peer (`docker pause` of node-a for 60 s and 180 s, then resume): the links come back to `active` and `last_error` is empty; `errors` keeps the count.
  - Dead connections (`ss -K` on the fabric ports 23600–23799): with only the state fix the links showed `active` but no grain arrived any more. With the stall check node-b logged `replication_restarted` for all four flows; 15 s after the cut all four were `active` with `restarts` 1 and 50 grains/s again.
  - Agent restart with `MXL_CLEANUP_ON_EXIT=false` (`fa-restart`: 4 flows, a GStreamer `mxlsrc` reader on one mirror): with 1.0.2 code every mirror flow got a new inode after restarting the destination agent and the reader stopped (462 → 487 frames, then nothing). With 1.0.3 the inodes stayed through restarts of the destination, the source and both, and the reader went on (462 → 1863 → 3304 → 4715 frames, none dropped). SIGTERM: exit 143 in 1.1 s, node deleted, 32 mirror flows left on the tmpfs.
  - `lag_grains`: 1–2 on healthy links (it was always 0 on a destination).
  - Destination agent paused for 20 s: both source links `error` with "no transfer completed for 2 s"; 20 s after resuming `active` again at 50 grains/s, `last_error` empty.
  - `/metrics` before 1.0.3: `replication_grains_total` always 0, `replication_bytes_total`, `replication_errors_total` and `replication_restarts_total` missing (the platform's alert and dashboard use them). Now set from the replication rows; two flows over 23 s: 1140 grains and 6.3 GB on both sides, errors 0, restarts 0.
- `tests/integration/shutdown.sh` checks registration, exit 143, node DELETE, and own-mirror removal.

## 6. Platform guideline G1–G14

v1.0.0 is the stable contract for the settings below. A later break needs v2.

| Item | Status | Evidence | What changed |
| --- | --- | --- | --- |
| G1 Configuration | met | `src/config/config.cpp:274`, `src/main.cpp:152` | Env, then file, then default. Unknown env keys ignored. Invalid values exit 78. Settings table in README. State under `STATE_DIR` (default `/config`). No secret settings |
| G2 MXL domains | met | `src/config/config.cpp:277`, `src/reconcile/controller.cpp:206`, `src/reconcile/controller.cpp:421` | `MXL_DOMAIN_SCAN_PATH` (alias `MXL_ROOT`, default `/Volumes/mxl`). One output domain and `history_duration` are N/A: mirrors are per source domain and copy the origin `options.json`. A `domain_def.json` with a different id is not overwritten. Cleanup deletes only this agent's mirrors |
| G3 NMOS identity | met | `src/util/uuid.hpp:30`, `src/nmos/node.cpp:134` | `NMOS_SEED` (default: `HOST_ID`, so existing node ids stay). No device, source, flow, sender, or receiver, so those ids are N/A. `NMOS_LABEL` and `NMOS_TAGS` are on the node. The host-id tag stays. No BCP-002 group hints exist |
| G4 Registry, no DNS-SD | met | `src/nmos/node.cpp:80`, `src/nmos/observer.cpp:363`, `src/config/config.cpp:358` | `NMOS_DNS_SD` defaults false: `pri` and `highest_pri` are `INT_MAX`, and the observer does not browse. Query defaults to the registry address and registration port + 1. Avahi is not required while DNS-SD is off |
| G5 Announce addresses | met | `src/util/net.cpp:72`, `src/nmos/node.cpp:73` | `NMOS_HOST_ADDRESS`, else the first non-loopback IPv4. Used for the node href, `api.endpoints[].host`, and the service href. `HOST_ID` stays the peer id, not the href host. No SDP, ICE, or SRT |
| G6 Ports | met | `src/main.cpp:127`, `src/reconcile/controller.cpp:172` | `WEB_PORT`, `NMOS_PORT` (WebSocket `NMOS_PORT+1`), `FABRIC_PORT_BASE`/`COUNT`. Bind failure of the web or NMOS listeners exits 75. Fabric ports come from the pool when a replication starts; one failure fails that replication |
| G7 Health and metrics | met | `src/reconcile/controller.cpp:634`, `src/ops/metrics.cpp:119` | `/livez`, `/readyz` (registration required only when a registry is configured), `/metrics` with prefix `mxl_fabrics_agent_` |
| G8 Clean shutdown | met | `src/main.cpp:143`, `src/reconcile/controller.cpp:200`, `src/nmos/node.cpp:197` | SIGTERM releases MXL resources, erases the node so nmos-cpp DELETEs it, DELETEs the node explicitly, removes own mirrors when `MXL_CLEANUP_ON_EXIT=true` (alias `CLEANUP_MIRRORS_ON_EXIT`), and exits 143 within `SHUTDOWN_TIMEOUT_S` (default 10). No child processes |
| G9 IS-05 | N/A | `src/nmos/node.cpp` | This agent has no senders or receivers and does not implement the connection API. It only reads other nodes' `/active` |
| G10 Config export | met | `src/reconcile/controller.cpp:883` | `GET /api/v1/config/export` and `POST /api/v1/config/import`. Existing config routes stay. Nothing is secret, so nothing is omitted |
| G11 Image and CI | met | `.github/workflows/container.yaml:28`, `docker/Dockerfile:86` | ghcr.io/leeo86/mxl-fabrics-agent. `main`: `git-<sha7>` and `nightly-dev`. Tag `vX.Y.Z`: `X.Y.Z`, `X.Y`, `X`. Those version tags are not moved. Image user 1000. `IPC_LOCK` is for RDMA, not root. OCI source, revision, licenses, and `io.dmf.mxl.revision` |
| G12 Kubernetes example | met | `deploy/mxl-fabrics-agent.yaml` | Host network is required (RDMA and peer fabric addresses). Standard env, `/livez` and `/readyz`, grace period above the shutdown timeout, MXL root hostPath, writable `/config`, no `hostIPC`, `IPC_LOCK` only |
| G13 Documentation | met | `README.md`, `CHANGELOG.md`, `SPECIFICATION.md` | Settings, ports, exit codes, API, platform runbook, 1.0.0 changelog. The spec matches the code |
| G14 Tests | met | `tests/unit/test_config.cpp`, `tests/integration/shutdown.sh`, `.github/workflows/ci.yaml` | Unit tests cover aliases, seed, tags, host address, query-port default, and import. Integration covers ready, SIGTERM, deregistration, and own-mirror removal |
