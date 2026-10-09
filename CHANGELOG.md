# Changelog

## 1.3.0

- A writer that restarts no longer stops its replications for good. MXL deletes a flow with its last writer, and a restarted writer (FlowXer, the test player) creates it again with a new inode. The source's reader stayed on the deleted flow, whose head stands still: the destination got no grain and did not rebuild, the source counted `get grain 11` (`MXL_ERR_FLOW_INVALID`) about 500 times a second (lab, re-created within milliseconds: 0 grains/s for good; with 6 s between the writers the destination released the pull and it worked). The source now checks the origin's data file once a second and at once when the reader reports the flow invalid; a changed or deleted file closes reader and initiator (`origin_changed`), and the next check opens the flow again for the same targets (`origin_reopened`). Lab: 50 grains/s again within about a second, no restart, no error.
- No log flood when a connection shuts down. MXL drops a target whose connection shut down (closed by the destination, or after a failed transfer: libfabric's `verbs` provider shuts an endpoint down on a completion error), and with none left every `MakeProgress` logged "No more targets available" (platform: 39,000 lines in 15 minutes; lab: 10,034 in 20 s with the destination stopped). The source now forgets those targets (one `source_connection_lost` line, `errors` + 1, `last_error`). The destination's next request adds the target again, and its target, which listens again after the shutdown, gets a new connection; a later remove no longer meets "Target with id … not found". Lab: 1 line in 20 s, replication back when the destination was.
- `link_down`. Each peer's fabric link is checked on every reconcile pass: the carrier of the local interface that holds `local_fabric_addr` (`/sys/class/net/<netdev>/carrier`), and that some interface holds the address at all ("source interface unavailable", the platform's irdma "Failed to get source interface information"). While it is down the destination drops its target and the source's connection and shows `link_down` with the reason, instead of `target_setup_failed` retries (platform: up to 975 restarts per flow during a 5 h outage of one link). When the link is back it sets up a fresh target and connection. The source's replications to that peer show `link_down` too, and its handshake answer makes the destination wait. One log line per change (`peer_link_down`, `peer_link_up`). `/api/v1/peers` has `link_up`, `link_error` and `fabric_interface`; the web UI shows a down link.
- Metrics: `peer_up{peer,interface}` is 1 only while the peer's control plane answers and the local fabric link to it has carrier (before: control plane only, label `peer`); `peers_up` counts the same. New: `replication_state{flow_id,peer,role,state}` (1 for the current state), `peer_rdma_retransmits_total{peer,device}` (`RetransSegs` of the RDMA device on the fabric interface; irdma only) and `origin_conflicts`.
- Retries of a failed target setup or handshake back off up to 30 s (was 10 s). `target_setup_failed` is logged when the error starts or changes, not on every retry.
- The handshake answer carries the source's state and reason (`state`, `error`). A source in `error` (connected, but no transfer completed for 2 s) makes the destination set its target up again after 1 s without a grain (1, 2, 4, 8 s in a row) instead of 5 s; the destination shows the source's reason in `last_error`.
- Two hosts with the same flow (a function moved to another host and left its domain behind; FlowXer's flow ids are deterministic): the destination replicates from the one whose origin is being written (`live` in the inventory: head index within 5 s of the current time), else one with a writer, and logs `origin_conflict` once per change. Before, the host that had the domain first kept it: the other one marked its own domain `conflict` and did not offer it, so a destination pulled the stale copy at 0 grains/s (platform: vmix 3924b34c on host-02 from host-01). A local domain is no longer `conflict` because a peer holds the same id.
- `fabric_endpoint` (info) logs the libfabric domain of each target and initiator when it is set up (for `verbs` the RDMA device), to compare with `rdma res`. The device in libfabric's "Async event for …" warning is not the one that raised the event: libfabric prints the first device of the fabric.
- `mxl-pattern-writer video … <recreate-after-s>` creates its flow again after that many seconds. `tests/integration/tcp_mesh.sh` checks a re-created flow (45 of 50 grains/s) and a destination that goes away (at most 20 "No more targets" lines in 3 s, then back at 45 grains/s).

## 1.2.3

- Continuous (audio) flows replicate completely. 1.2.2 sent one sync batch per transfer and at most one transfer per pass of the fabric thread (2 ms). With the ST 2110 gateway's batch of 48 samples (1 ms) half of a 48 kHz flow arrived (lab, `tcp` and `verbs`), over `verbs` on the platform's E810 8 % (815 transfers in 10 s, `lag_grains` growing, 89 % zeros for the reader of the mirror). Now one transfer carries every sample written since the last one, up to the target's bounce buffer entry (MXL sizes it from the mirror's sync batch, 10 ms by default), and it starts one pass after the last one completed: over `verbs` MXL's target posts the receive for the next immediate only when its agent drains the last transfer, and a transfer that comes sooner meets "receiver not ready" (soft-RoCE, four audio flows: about 150 per second with 1.2.2, none now). Lab (one host, `tcp` and soft-RoCE, 2 ch 48 kHz, 200 ms history, one and four flows plus 1080p50): 48,000 of 48,000 samples/s on each mirror, none missed, the mirror about 4 ms behind the origin (1.2.2: about 22,500 samples/s, up to 200 ms behind); video unchanged at 50 grains/s; agent CPU unchanged.
- `replication_bytes_total` of a continuous flow counts all channels (it counted 4 bytes per sample: 192 B for a transfer of 48 samples of 2 ch float, which carries 384 B).
- The container workflow runs once per ref; a newer run cancels the older one. The v1.2.2 tag started it twice, and the second run pushed another image over the tag's digest.
- README: keep `RT_PRIORITY` below 50. The kernel's threaded IRQs run at `SCHED_FIFO` 50, and at equal priority a NIC IRQ thread waits for the agent's pass (the platform uses 49).
- `mxl-pattern-writer audio` writes a 2-channel 48 kHz pattern flow in batches of 48 samples, `mxl-pattern-writer samples` counts the correct samples of its mirror; `tests/integration/tcp_mesh.sh` requires 45,000 of 48,000 per second.

## 1.2.2

- `RT_PRIORITY` works for the non-root agent. The binary carries `cap_sys_nice` and `cap_ipc_lock` as permitted file capabilities (`setcap …+p`), and the agent raises them into its effective set at start, before any thread. Kubernetes `capabilities.add` only puts a capability into the bounding set; without an effective `SYS_NICE` the fabric thread got no `SCHED_FIFO` (platform: `rt_priority_failed "Operation not permitted"` on every agent). The effective bit is not set on the file, so a runtime that does not grant `SYS_NICE` still starts the agent (it then logs `rt_priority_failed` as before). The `startup` log line lists the effective capabilities (`capabilities`).
- `deploy/mxl-fabrics-agent.yaml` adds `SYS_NICE` to `capabilities.add`.

## 1.2.1

- A receiver's IS-05 `/active` is read at `<control href>/single/receivers/<id>/active` without a doubled slash. Devices advertise the href with a trailing `/` (nmos-cpp, FlowXer); FlowXer up to 9.16.33 answered the `//single/…` form with 404, so the agent took all its receivers as not routed and replicated none of their flows (small platform: the vision mixer's inputs stayed frozen mirrors, Program rendered no frames).
- A receiver whose `/active` cannot be read is logged once (`nmos_active_unavailable` with URL and HTTP status) until it works again. It counts as not routed, which was silent before.

## 1.2.0

- Transfer pacing is removed again. On the platform's E810 mesh over `verbs` it multiplied retransmissions about 1000-fold and replication broke (517 link restarts in 69 minutes, readers without grains); unpaced, the same links ran at 50 grains/s. MXL's target keeps one receive posted for the immediate data that ends every transfer and posts the next only when the agent reads the completion; with 8 batches per grain the next batch usually arrived before that (receiver not ready). MXL's public API has no transfer without an immediate, so the agent cannot work around it. `TRANSFER_PACING`, `TRANSFER_PACING_SPREAD` and `TRANSFER_PACING_BATCHES` are ignored (a set `TRANSFER_PACING` other than `off` logs `transfer_pacing_removed`); the gauges `transfer_pacing_batches` and `transfer_pacing_spread` are gone.
- Unchanged from 1.1.0: links whose origin writes nothing are not rebuilt (`origin_head`), `lag_grains` is 0 while that head stands still, and `grain_transfer_seconds{provider}` (now transfer to completion of one grain).

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
