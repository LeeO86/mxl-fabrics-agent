# Changelog

## 1.1.0

- Optional transfer pacing: `TRANSFER_PACING=frame` sends each complete video or data grain in `TRANSFER_PACING_BATCHES` slice batches (default 8) whose starts are spread over `TRANSFER_PACING_SPREAD` of the grain duration (default 0.5, 0.1–0.9), instead of one burst at line rate. For receivers whose NIC cannot move bursts from two ports into memory at once (on the platform: E810 at PCIe Gen3 x8 discarding RoCE packets). Uses MXL's slice-range transfer; no MXL change. Adds up to the spread to the latency (10 ms at 50p with the default). Default `off`: nothing changes unless it is set. Restart required.
- `grain_transfer_seconds{provider}` histogram (source grains, first transfer to completion; with pacing the spread included) and the gauges `transfer_pacing_batches` (0 = off) and `transfer_pacing_spread`. The Grafana dashboard shows the p95 transfer time.
- A destination counts a grain in `replication_grains_total` and `replication_bytes_total` once, also when it arrives in several batches.
- A link whose origin writes nothing is no longer rebuilt. 1.0.3 rebuilt a destination without new grains whenever a writer held the origin flow; a decklink input without a signal holds its flow but writes nothing, so its links were rebuilt every 5–40 s (16 times in 15 minutes on the platform) and `replication_restarts_total` rose. The source now returns the origin head index in the handshake answer (`origin_head`), and the destination rebuilds only when the origin moved on after its last grain (or the link is `error`). With a source before 1.1.0 the 1.0.3 rule stays. `lag_grains` is 0 while that origin head stands still (it grew with the TAI index before).

## 1.0.3

- Mirror flows survive an agent restart when `MXL_CLEANUP_ON_EXIT=false`. MXL deletes a flow when its last writer is released or its instance destroyed, so on SIGTERM the agent removed every mirror flow and created it anew at the next start (new inode). Readers on the host stayed on the deleted flow until they were activated again or restarted (platform: mxl-webrtc-monitor, mxl-multiviewer after `kubectl rollout restart`). Now the agent closes its fabric connections and leaves its mirror writers and the MXL instance to the process exit; the next start re-opens the same flows. Lab: restart of the destination agent, the source agent and both, inodes unchanged, a GStreamer reader on a mirror kept reading (before: stopped for good); SIGTERM still exits 143 in about 1 s.
- `lag_grains` (`replication_lag_grains`) is measured on cross-host links. It compared the mirror head with a source head on the same agent, which a destination never has, so it was always 0. Now it is the flow's current TAI index minus the mirror head while a writer holds the origin flow (origin writers commit at the current index); continuous flows count transfer batches. Lab: 1–2 grains on healthy 1080p50 links.
- A source link that is connected but completes no transfer for 2 s is `error` ("no transfer completed for 2 s"). MXL counts a failed transfer (RDMA queue pair out of retries) as never finished, so the initiator stayed `NOT_READY`, sent nothing more and showed `active` with a frozen head. The destination's rebuild (below) replaces the dead connection.
- New mirror `domain_def.json` files carry `description` and `tags` (BCP-007-03 requires them; mxl-st2110-gateway 1.0.2 skipped mirror domains without them). Existing files are not rewritten.
- A dead link is set up again. A destination without a new grain for 5 s while it is in `error` or its source flow is being written (a peer that hung, a queue pair out of retries, a closed connection that reports nothing) tears its target down and repeats the handshake; the source replaces the dead connection with the new target. `restarts` counts it, the log says `replication_restarted`. The wait doubles with each rebuild in a row (5, 10, 20, 40 s). Before, such a link stayed `error` (or even `active` without grains) with `restarts` 0 until the agent restarted.
- `last_error` is cleared when a link works again: a source or destination pass without a new error is `active` again (a destination in `error` also needs new grains), and a successful handshake clears `recv` or `http …`. The `errors` count keeps the history.
- `/metrics` exports `replication_grains_total`, `replication_bytes_total` and `replication_errors_total` per flow, peer and role, and `replication_restarts_total` per destination. Before, `replication_grains_total` was always 0 and the other three were missing. `replication_errors_total` has no `kind` label (SPEC §12 now says so; the Grafana error panel groups by flow and role); the histograms `grain_transfer_seconds` and `setup_seconds` and `nmos_poll_errors_total` are marked "not implemented yet".

## 1.0.2

- The image contains `ibverbs-providers`, the user-space RDMA drivers (`irdma` for Intel E810, `mlx5` for ConnectX, `rxe`) and `/etc/libibverbs.d`. Without them libibverbs found no device even with `/dev/infiniband` mapped, and an agent with `DEFAULT_PROVIDER=verbs` exited with `no_fabric_provider`. `ibverbs-utils` adds `ibv_devices` and `ibv_devinfo` to check what the container sees. Lab check over soft-RoCE (`rxe` on one host, two agents): 4 flows of 1080p50 replicate over `verbs` at 50.2 grains/s, lag 0, no errors.
- A connected source link stays `active`. `mxlFabricsInitiatorMakeProgressNonBlocking` returns `MXL_ERR_NOT_READY` while transfers are still in flight, which on `verbs` is most passes; the agent showed `pending` for those, and the destination copied it, so links that moved 50 grains/s flipped between `active` and `pending` in `/api/v1/replications` and the web UI (on `tcp` too, less often). Only a link that has not connected to its current target is `pending`. The `replications_active` metric counted both states and does not change.

## 1.0.1

- A replication target that the destination requests again with the same target info is kept. The destination repeats its `POST /replications` on every reconcile pass, and the source removed and re-added the target each time, which dropped the connection: links showed `pending`, and a flow with a 200 ms ring (MXL's default, 10 grains at 50p) lost half its grains while the initiator reconnected (lab run 2026-10-03). The integration test now replicates a 1080p50 flow with a 200 ms ring over `tcp`: 50.3 of 50 grains/s, none missed (26.3 before).
- The agent raises its open-file soft limit to the hard limit at start. A mirror keeps one descriptor per grain, and with Docker's default soft limit of 1024 eager mirrors of about 30 flows failed with "Too many open files" (`mirror_writer_failed`, `mxlCreateFlowWriter failed (1)`).

## 1.0.0

Stable platform contract. A later breaking change needs v2.

New settings, with the previous names kept as aliases:

- `MXL_DOMAIN_SCAN_PATH` is the MXL parent directory. `MXL_ROOT` is the alias. Default remains `/Volumes/mxl`.
- `MXL_CLEANUP_ON_EXIT` removes this agent's own mirror directories on shutdown. `CLEANUP_MIRRORS_ON_EXIT` is the alias. Default remains false.
- `NMOS_SEED` sets the UUIDv5 node id. Unset, the id is still UUIDv5 of `HOST_ID`.
- `NMOS_LABEL` sets the node label. Default is `HOST_ID`.
- `NMOS_TAGS` adds a JSON object of tags to the node. The host-id tag is still set.
- `NMOS_DNS_SD` defaults to **false**. DNS-SD browse and mDNS advertisement are both off unless this is `true`. A deployment that relied on an empty `NMOS_REGISTRY_ADDRESS` to browse must set `NMOS_DNS_SD=true`.
- `NMOS_HOST_ADDRESS` is the IP literal announced in the node href, `api.endpoints[].host`, and the service href. Unset, the first non-loopback IPv4 is used. `HOST_ID` remains the fabrics peer id.
- `NMOS_QUERY_PORT`, when unset, is `NMOS_REGISTRY_PORT + 1` rather than a fixed 3211.
- `STATE_DIR` (default `/config`) is where the process stores its configuration file.
- `SHUTDOWN_TIMEOUT_S` (default 10) is the SIGTERM budget.

Behaviour:

- SIGTERM and SIGINT exit **143** after MXL resources are released, the node is removed from the registry, and own mirrors are deleted when cleanup is on.
- A listen port that cannot be bound exits **75**.
- `/readyz` is 200 only when the scanner is running and, if a registry is configured, the node is registered.
- `GET /api/v1/config/export` and `POST /api/v1/config/import` save and restore configuration. This agent has no secret settings.
- An existing mirror `domain_def.json` with a different id is not overwritten.
- The image is uid 1000. `IPC_LOCK` is for RDMA registration. OCI label `io.dmf.mxl.revision` is the MXL commit `218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7`. Version tags `X.Y.Z`, `X.Y`, and `X` are not moved. `nightly-dev` remains the moving tag on `main`.
