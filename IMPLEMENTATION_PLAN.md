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

## 3. Process

One thread reconciles scanner, peer, and NMOS snapshots into mirror plans (`src/mirror/lifecycle.cpp`) and pull requests. Each MXL domain that the agent opens has a fabric thread that serialises MXL and Fabrics calls, drains target completion queues, and transfers new grains or samples. Peer inventory polling and the NMOS observer run on their own threads. The HTTP server is another thread.

`verbs` setup failure with `PROVIDER_FALLBACK=tcp` retries that link on `tcp` and reports `fallback: true`.

## 4. Tests

- Unit tests cover config precedence, local-node matching, IS-05 demand derivation, the mirror state machine, include/exclude filters, inventory classification, and handshake idempotency.
- `tests/integration/tcp_mesh.sh` runs two agents on one machine with the `tcp` provider: eager mirror before activation, identical grain indices, release after grace, peer down, resume, and `stale_reference`.
- Hardware checks in spec §15.4 (sustained `verbs` on E810 and ConnectX) are not run in CI.
