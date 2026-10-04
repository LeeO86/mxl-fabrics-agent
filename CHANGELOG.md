# Changelog

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
