# mxl-fabrics-agent — Specification

Status: Draft v0.1 (for implementation by Claude Code in a new, empty repository)
Working name: `mxl-fabrics-agent` (final repository name still open)
Sibling projects this spec aligns with: `LeeO86/mxl-decklink`, `LeeO86/mxl-st2110-gateway`

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.

---

## 1. Purpose and scope

`mxl-fabrics-agent` is a per-host infrastructure container that makes MXL flows
produced on one host available to MXL readers on other hosts, using the MXL 1.1
Fabrics API (libfabric; `verbs` for RoCEv2, `tcp` as fallback).

It is driven entirely by standard NMOS state:

- Media functions (e.g. mxl-decklink, mxl-st2110-gateway, Strom) keep their own
  NMOS nodes and their own BCP-007-03 senders and receivers. The agent does not
  own, proxy, or modify any media function's NMOS resources.
- A controller connects a receiver to a sender with plain IS-05
  (`mxl_domain_id`, `mxl_flow_id`). When the receiver lives on a different host
  than the source domain, the agent on the receiver's host detects that and
  arranges replication from the source host.
- Media functions do not link libfabric or the Fabrics API and need no changes,
  apart from the generic MXL behaviours listed in §11 (domain scanning and
  retry when a flow is not yet present).

Design principles:

1. **Standards only on the control surface.** IS-04 v1.3, IS-05 v1.2,
   BCP-007-03. No proprietary extension is required of controllers or media
   functions.
2. **The agent is read-only towards NMOS.** It never PATCHes any IS-05 endpoint
   and never registers senders or receivers. It registers exactly one minimal
   Node of its own (§6.3) for peer discovery.
3. **The FlowWriter API of media functions is never intercepted.** Replication
   happens beneath MXL: the agent is an ordinary MXL reader on the source host
   and an ordinary MXL writer on the destination host.
4. **Separation of metadata and data.** Flow structure is mirrored early;
   grain data moves only while a receiver needs it (§7).

Out of scope for v1: compressed formats, NAT traversal, more than one fabric
provider per peer link at the same time, authentication (IS-10).

---

## 2. Terminology

| Term | Meaning |
| --- | --- |
| Host | One physical machine with one MXL tmpfs root and one agent instance. |
| MXL root | The tmpfs mount containing all MXL domains of the host (e.g. `/Volumes/mxl`). |
| Local domain | A domain created by a media function on this host. |
| Mirror domain | A domain created by the agent on this host that carries the identity of a domain on another host. |
| Origin flow | A flow in a local domain, written by a media function. |
| Mirror flow | A flow in a mirror domain, written by the agent, with the same flow id and geometry as the origin flow. |
| Replication | An active Fabrics transfer of one origin flow to one or more mirror flows on other hosts. |
| Source agent | The agent on the host that holds the origin flow (runs the Fabrics initiator). |
| Destination agent | The agent on the host that needs the flow (runs the Fabrics target). |
| Demand | The set of (domain id, flow id) pairs that enabled receivers on this host need and that are not in a local domain. |

---

## 3. Architecture overview

```
 Host A (source)                                Host B (destination)
 ┌───────────────────────────────┐              ┌───────────────────────────────┐
 │ media function (NMOS sender)  │              │ media function (NMOS receiver)│
 │   └─ MXL writer ─┐            │              │            ┌─ MXL reader ─┘   │
 │                  ▼            │              │            ▼                  │
 │ /Volumes/mxl/<local domain>   │              │ /Volumes/mxl/mirror-<A-domain>│
 │                  │            │              │            ▲                  │
 │ mxl-fabrics-agent│            │   RDMA       │ mxl-fabrics-agent             │
 │   MXL reader ─► Initiator ════╪══════════════╪══► Target ─► MXL writer       │
 │   control API ◄───────────────┼── REST/SSE ──┼───────────── control API      │
 └───────────────┬───────────────┘              └──────────────┬────────────────┘
                 │                IS-04 Query API (WebSocket)  │
                 └──────────────────► NMOS registry ◄──────────┘
```

Internal components (one process, C++):

1. **Domain scanner** — watches the MXL root with inotify, maintains the local
   inventory (§5).
2. **NMOS observer** — subscribes to the IS-04 Query API, tracks MXL receivers
   on local nodes, reads their IS-05 `/active`, derives demand (§6).
3. **Peer manager** — discovers peer agents, exchanges inventories, holds the
   peer link map (§8).
4. **Mirror manager** — creates, updates and removes mirror domains and mirror
   flows (§7).
5. **Replication engine** — owns all Fabrics instances, initiators and targets,
   runs the progress and completion loops (§9).
6. **Control API** — agent-to-agent REST + SSE (§8.3).
7. **Web/ops server** — health, status, metrics, admin UI (§12).
8. **NMOS node** — minimal nmos-cpp Node for discovery (§6.3).

A single reconciliation loop combines scanner, observer and peer state into a
desired state and drives mirror manager and replication engine towards it. All
event sources (inotify, WebSocket, SSE) only trigger an earlier reconciliation;
correctness MUST NOT depend on any single event being received.

---

## 4. Technology and build

- Language: C++20. Build: CMake ≥ 3.24, Ninja. Compilers: GCC ≥ 12 or Clang ≥ 16.
- MXL: `dmf-mxl/mxl` pinned to an exact tag/commit of the v1.1 line, built with
  `-DMXL_ENABLE_FABRICS_OFI=ON`. The pin MUST be a single variable in the
  Dockerfile and CI. Before choosing the pin, check upstream for fixes of these
  known issues and prefer a revision that contains them:
  - colliding endpoint ids when many targets are set up concurrently;
  - completion-queue overflow on targets that drain one completion per call
    (on `verbs` this tears down the queue pair).
- libfabric ≥ 2.3 (needed for `FI_SOCKADDR_IP` with the `tcp` provider), with
  the `verbs` and `tcp` providers enabled; rdma-core userspace (libibverbs,
  librdmacm, irdma provider).
- NMOS: Sony nmos-cpp, pinned commit (same approach as mxl-decklink), used for
  the agent's own Node and for IS-04 Query API / IS-05 client access where its
  client utilities fit; otherwise the C++ REST SDK that nmos-cpp already depends on.
- Web UI: Vue 3 SPA built with Node.js ≥ 20, embedded into the binary
  (same pattern as mxl-decklink).
- Tests: doctest (vendored), shell integration tests.
- Follow the actual headers of the pinned MXL version. Where this spec
  paraphrases an MXL or Fabrics API and the real API differs, follow the real
  API and record the deviation in `IMPLEMENTATION_PLAN.md` (same convention as
  mxl-decklink).

Repository layout (mirrors the sibling repos):

```
.github/workflows/   CI and image publishing
cmake/               find modules, helpers
deploy/              Kubernetes manifests, Grafana dashboard, Prometheus examples
docker/              Dockerfile, docker-compose demo
src/                 C++ sources
tests/               unit + integration
third_party/         doctest
web/                 Vue SPA
AGENTS.md
IMPLEMENTATION_PLAN.md
README.md
SPECIFICATION.md     (this file)
LICENSE              MIT
```

---

## 5. Local inventory (domain scanner)

### 5.1 Scanning

- The agent MUST mount the whole MXL root read-write (`MXL_ROOT`, default
  `/Volumes/mxl`). Startup MUST fail with exit 78 if `MXL_ROOT` does not exist,
  and SHOULD warn if it is not a tmpfs.
- It discovers domains as direct subdirectories of `MXL_ROOT` that contain a
  `domain_def.json`. The domain identity is the `id` in `domain_def.json`, not
  the directory name (same convention as mxl-decklink and Strom).
- For each domain it reads `options.json` (in particular `history_duration`,
  which defines ring depth domain-wide) and enumerates flows with their
  `flow_def.json`.
- A flow is **active** if its writer lock is held (use the MXL API for this
  where available; otherwise the advisory `flock` convention MXL uses for stale
  flow detection). Inactive flows are listed but never replicated.
- inotify on `MXL_ROOT` and each domain directory triggers re-scans; a full
  re-scan also runs every `SCAN_INTERVAL_MS` (default 2000).

### 5.2 Classification

Each domain is classified as:

- **local** — no mirror marker (see §7.2);
- **mirror** — carries the agent's mirror marker; these are never exported in
  the inventory (loop prevention);
- **conflict** — a mirror marker that does not belong to this agent. Conflicts
  are reported (status, metric, log) and excluded from replication. A local
  domain whose id another host also holds stays local (since 1.3.0); a
  destination that sees a flow on several hosts picks one (§8.4).

### 5.3 Inventory record

Per local domain: `domain_id`, `path`, `options` (verbatim `options.json`),
and per flow: `flow_id`, `flow_def` (verbatim JSON), `format`
(discrete/continuous), `media_type`, `active`, `live` (a writer holds it and
its head index is within 5 s of the current TAI time), `grain_rate` or
`sample_rate`, `ring_depth`, `payload_size`.

---

## 6. NMOS observation and demand

### 6.1 Identifying local nodes

A Node registered in the registry is considered local to this host if any of:

1. the host part of any of its `api.endpoints` matches an IP address of this
   host (default and expected with host networking);
2. its `hostname` matches the host's hostname;
3. its `id` or `hostname` is listed in `LOCAL_NODE_IDS` / `LOCAL_NODE_HOSTNAMES`;
4. the host part of any of its `api.endpoints` is an IP address inside
   `LOCAL_NODE_CIDRS`, or a DNS name that resolves to such an address.

Rule 4 is for media functions that run on the Kubernetes pod network of the
same node as the agent. Those Nodes advertise a pod IP, or sometimes a
Service DNS name, which is not a host interface address, and their
`hostname` is the pod name. The platform sets `LOCAL_NODE_CIDRS` to that
node's pod CIDR. The agent's own Node is excluded before any of these rules.
Lookups are time-bounded and cached for a short time; a name that does not
resolve is not local. An empty `LOCAL_NODE_CIDRS` disables the rule.

### 6.2 Observing receivers

- The agent opens an IS-04 Query API WebSocket subscription on `/receivers`
  filtered by `transport=urn:x-nmos:transport:mxl`, plus subscriptions on
  `/devices` and `/nodes` to resolve receiver → device → node and the device's
  IS-05 control href (`urn:x-nmos:control:sr-ctrl/v1.2`, falling back to v1.1).
- For every MXL receiver on a local node it reads IS-05
  `GET .../single/receivers/{id}/active` when:
  - the receiver's IS-04 `subscription` changes, or its `version` changes;
  - on every reconciliation tick (`NMOS_POLL_INTERVAL_MS`, default 1000),
    because a controller may change transport params without the IS-04
    subscription changing.
- The registry Query API is `NMOS_QUERY_ADDRESS` (default: `NMOS_REGISTRY_ADDRESS`)
  on `NMOS_QUERY_PORT` (default: `NMOS_REGISTRY_PORT + 1`).
- `NMOS_DNS_SD` defaults to false. DNS-SD browse and mDNS advertisement are
  then both off, and Avahi or D-Bus is not required. `NMOS_DNS_SD=true` is the
  only mode that browses `_nmos-query._tcp`.

### 6.3 The agent's own Node

- The agent registers one IS-04 v1.3 Node with no devices, senders or receivers.
  There is no device, so there are no device, source, flow, sender, or receiver ids.
- The node id is UUIDv5 of `NMOS_SEED` when that is set, otherwise UUIDv5 of
  `HOST_ID` (the same id this agent has always used). The same seed gives the
  same id after a restart.
- `NMOS_LABEL` is the node label (default `HOST_ID`).
- `NMOS_TAGS` (a JSON object of string arrays) is added to the node. The tag
  `urn:x-leeo86:mxl-fabrics-agent:host-id: ["<HOST_ID>"]` is always set. This
  agent has no BCP-002 group hints.
- The node `href`, `api.endpoints[].host`, and the service href are the IP
  literal `NMOS_HOST_ADDRESS` (default: the first non-loopback IPv4). They are
  never a hostname, `0.0.0.0`, or `127.0.0.1`.
- Its `services` array contains one entry:
  `{"href": "http://<NMOS_HOST_ADDRESS>:<WEB_PORT>/api/v1", "type": "urn:x-leeo86:service:mxl-fabrics-agent/v1.0", "authorization": false}`.
- This Node is the discovery mechanism for peer agents (§8.1). It is optional
  only if `PEERS` is fully specified statically.
- With a registry configured, `/readyz` stays 503 until the registration
  client has registered the node or the Query API lists it. `NMOS_DNS_SD=false`
  and an empty registry address do not wait for registration.
- On shutdown the node resource is removed from the nmos-cpp model so the
  registration client sends DELETE, and the agent also DELETEs
  `/x-nmos/registration/v1.3/resource/nodes/{id}`.

### 6.4 Deriving demand

A receiver contributes to demand if, in its IS-05 `/active`:

- `master_enable` is `true`, and
- `transport_params[0].mxl_domain_id` is set and is **not** a local domain on
  this host, and
- `transport_params[0].mxl_flow_id` is set.

`sender_id` is informational only and MAY be null. The source host is resolved
by looking up `mxl_domain_id` in the peer inventories (§8). If no peer holds the
domain, the demand entry is `unresolved` (status + metric), and resolution is
retried on every tick.

Demand entries carry the set of local receiver ids that need them (for
reference counting and the UI).

---

## 7. Mirror domains and mirror flows

### 7.1 Purpose

A mirror flow lets a receiver on the destination host open the flow
successfully before any grain data has arrived. The reader then simply sees no
new grains yet, which every MXL reader already has to handle. This removes the
activation race between IS-05 activation and replication start.

### 7.2 Layout and identity

- Mirror domain path: `<MXL_ROOT>/mirror-<source-domain-id>/` — a sibling of the
  local domains, so media functions scanning `MXL_ROOT` find it.
- `domain_def.json` carries the **source** domain id, a label, a description
  and empty tags (BCP-007-03), plus a marker object:
  `"x-mxl-fabrics-agent": {"mirror": true, "source_host_id": "...", "owner_host_id": "<HOST_ID>"}`.
  Readers ignore unknown fields; the agent uses the marker for classification.
- `options.json` is copied verbatim from the source domain, so ring geometry
  (`history_duration`) is identical.
- Mirror flows are created with the source's `flow_def.json` verbatim (same
  flow id, format, rate, geometry) and are owned by an MXL FlowWriter held by
  the agent for as long as the mirror exists. Holding the writer keeps the lock
  so stale-flow garbage collection does not remove it.

### 7.3 Mirror modes

`MIRROR_MODE`:

- `eager` (default): mirror every active origin flow of every peer, without
  transferring data. Costs tmpfs memory (one ring per flow per mirroring host).
- `on-demand`: create a mirror flow only when a demand entry exists. Readers
  may then briefly see "flow not found" and rely on their retry path.
- `MIRROR_INCLUDE` / `MIRROR_EXCLUDE` (comma-separated domain ids or flow ids)
  restrict eager mirroring.

Before creating a mirror flow the agent checks free tmpfs space (`statvfs`) and
keeps at least `TMPFS_RESERVE_MB` (default 512) free. If not possible, the
mirror is not created, and status/metric/log report `insufficient_space`.

### 7.4 Lifecycle

- Origin flow appears/becomes active on a peer → mirror flow created (eager) or
  on demand.
- Origin flow becomes inactive or disappears → replication stopped, mirror flow
  kept for `MIRROR_GRACE_S` (default 10) to bridge writer restarts, then
  removed. Removal MUST NOT happen while a local reader is known to be enabled
  on it; in that case the mirror stays and is reported as `orphaned`.
- Peer unreachable → mirrors stay (readers see no new grains), status `peer_down`,
  replication resumes automatically when the peer returns.
- Origin writer restarts → MXL deletes the flow with its last writer and the
  next writer creates it again with a new inode. The source checks the origin's
  data file once a second (and at once on `MXL_ERR_FLOW_INVALID`); a changed or
  deleted file closes its reader and initiator, and the next check opens the new
  flow for the same targets. The destination keeps its target and mirror when
  the new flow has the same definition (grains go on in the same mirror file).
- Origin created again with another definition under the same flow id (1.7.0):
  when the origin's `flow_def.json` in the source's inventory differs from the
  mirror's own in more than its descriptive fields (`label`, `description`,
  `tags`, `parents`, `version`, `source_id`, `device_id`), the mirror's layout
  (rate, format, media type, geometry) is wrong. The destination drops its
  target and the source's connection to it, releases its writer, removes the
  flow's files and creates the flow again from the new definition on the same
  pass (log `mirror_origin_changed`; rebuild reason `origin_config_changed`).
  Readers are not waited for: the old layout is wrong for them too, and they get
  `MXL_ERR_FLOW_INVALID`. At most one such rebuild per flow per 5 s.
- Format change at the source (mxl-decklink mints a new flow UUID): the new flow
  is mirrored like any new flow; the old one follows the "disappears" rule.
  Receivers still pointing at the old flow id are reported as `stale_reference`
  (status + metric). The agent does not reconnect them; that is the
  controller's job.
- On agent startup, existing mirror domains with this agent's marker are
  adopted (re-opened as writer) or removed if no longer wanted. Mirror domains
  with a foreign `owner_host_id` are left untouched and reported as conflict.
- Whole mirror domains (1.4.0): `same_node` (the origin domain is on this node)
  and `orphan` (no up peer lists the origin and its source peer is not a known
  peer that is down) are removed at once; `idle` (no demand, no held
  replication, not eager-mirrored) after `MIRROR_IDLE_GRACE_S` (default 300,
  `0` never); `active` stays. A mirror of a local origin is never created.
  Removal closes the agent's writers and targets first, then removes the flows
  and the directory, and MUST NOT happen while a process other than the agent
  maps one of its files (`/proc/<pid>/maps`, device and inode) or while the
  agent cannot check every process. Orphans are decided only 30 s after start
  and, with NMOS, while the registry answers. `MIRROR_CLEANUP_DRY_RUN` only logs.
- Single mirror flows (1.6.0): in a mirror domain that stays, a flow whose
  origin is missing (the domain's source peer is up and no up peer offers the
  flow) is removed once it has been missing for `MIRROR_IDLE_GRACE_S` (`0`
  never), not in the first 30 s after start and, with NMOS, only while the
  registry answers. The clock restarts when the flow is offered again, its
  source goes down, or the agent restarts. Removal releases the agent's writer
  of the flow and removes `<flow id>.mxl-flow`; it MUST NOT happen while a
  process other than the agent maps a file of that flow directory (the same
  `/proc/<pid>/maps` check, per flow) or while the agent cannot check every
  process, and the check runs again right before the files go. Same ownership
  rules as whole domains. `MIRROR_CLEANUP_DRY_RUN` only logs
  (`mirror_flow_would_remove`). The flow's health becomes `absent` with reason
  `origin_missing`.
- On clean shutdown (SIGTERM) the agent stops replications and, if
  `MXL_CLEANUP_ON_EXIT=true` (alias `CLEANUP_MIRRORS_ON_EXIT`, default false),
  removes only mirror directories it owns. With `false` it keeps the mirror
  flows: MXL deletes a flow when its last writer is released or its instance
  destroyed, so the agent leaves both to the process exit, and the next start
  re-opens the same flows (readers keep them; a re-created flow would leave
  readers that do not reopen on `MXL_ERR_FLOW_INVALID` on the old one). An
  existing `domain_def.json` whose id does not match the mirror is not
  overwritten.

### 7.5 Mirror health (stale, rebuilt)

The agent reports; it never re-routes. Choosing a source for a receiver stays with
the controller (IS-05), so the agent only tells the platform when a mirror flow no
longer carries the source.

Per destination mirror flow, on every reconciliation tick:

- **Grains are due** while a replication of the flow is wanted (a receiver, or a
  held replication) and the origin is live in the source's inventory (a writer
  holds it and its head is within 5 s of now). While the source peer or its
  fabric link is down, grains stay due as long as the demand stays.
- **Age of the newest grain** is the mirror's head index against the current TAI
  index at the flow's rate (samples ÷ sample rate for continuous flows), read
  from the mirror's data file. It includes the replication lag.
- **`stale`**: grains are due, no new grain came for `MIRROR_STALE_MS` (default
  2000; `0` never) and the origin has newer ones. The source's
  `/api/v1/inventory` carries each local flow's `head_index` read when it
  answers (not part of `revision`); without it (an older source) the origin's
  `live` flag stands in. With the peer or its link down the origin is taken as
  still written. Reasons: `no_new_grains`, `no_first_grain`, `peer_down`,
  `link_down`. When grains first become due the full wait applies, so a mirror
  that holds old grains is `pending`, not `stale`, at once.
- Other states: `live` (a grain younger than the threshold), `pending` (due,
  within the first wait), `idle` (nothing due: `no_demand`, `origin_inactive`,
  `origin_not_live`, `origin_missing`, `peer_down`, or `origin_silent`: the
  origin has no newer grain, e.g. a writer without input signal), `absent`
  (the flow is not on disk).
- **`rebuilt`**: the mirror's data file is a new one (another inode, or back
  after it was gone). MXL readers keep the file they opened and see no new
  grain on a flow that was created again, so every reader of the mirror needs a
  fresh IS-05 activation. The reason is why the agent removed it
  (`origin_gone`, `origin_missing`, `no_demand`, `excluded`, `mirror_domain_<idle|orphan|same_node|api>`,
  `origin_config_changed`, `api`) or `replaced` when something else did. Counts
  and times live in memory: an agent restart starts them again.

**Rebuild rules.** Whether an origin restart rebuilds a mirror (new files, new
inode) or keeps it (grains go on in the same file) is decided by what the
destination sees of the origin in the source's inventory (polled every
`PEER_POLL_INTERVAL_MS`, sooner on its SSE events), not by the restart itself:

| What the destination sees | Mirror | Rebuild reason when it comes back |
| --- | --- | --- |
| Origin listed again with the same definition, whatever the gap, before one of the rules below fires | kept | — |
| Origin listed but inactive (a writer killed, its files left): with a receiver on this host | kept (`orphaned` after `MIRROR_GRACE_S`) | — |
| the same without a receiver, longer than `MIRROR_GRACE_S` (default 10 s) | removed, no reader check | `origin_gone` |
| Origin flow not listed (MXL deleted it with its last writer), its domain still listed, longer than `MIRROR_IDLE_GRACE_S` (default 300 s) and no process maps the mirror flow | removed | `origin_missing` |
| Origin domain not listed (the function removed it on exit) on a peer poll, and no process maps a file of the mirror domain at the next reader check (every 2 s) | removed at once | `mirror_domain_orphan` |
| Origin listed with another definition (§7.4) | rebuilt at once, readers or not | `origin_config_changed` |
| Source peer down, link down, destination agent restart (`MXL_CLEANUP_ON_EXIT=false`) | kept | — |
| `MIRROR_MODE=on-demand` and the demand gone for `RELEASE_GRACE_MS` | removed | `no_demand` |
| `POST /api/v1/mirrors/{domain_id}/{flow_id}/rebuild` | rebuilt at once | `api` |

Given the timing the outcome is deterministic. A restart that removes the whole
origin domain races the peer poll and the reader check: a gap shorter than
one poll may go unseen, and a mirror that a reader maps at the check stays.

**Test hook.** `POST /api/v1/mirrors/{domain_id}/{flow_id}/rebuild` rebuilds one
mirror flow of this agent under the same ids, as an origin with a new
definition would: it drops the flow's target and the source's connection to it,
releases the writer, removes the flow's files and creates the flow again from
its own `flow_def.json`; the next reconciliation pass sets up a new target and
replication resumes. Readers that map the old file get `MXL_ERR_FLOW_INVALID`
once no new grain comes and must open the flow again. Off unless
`MIRROR_ADMIN_REBUILD=true` in the environment, read at start. It is
environment-only, so only the deployment opens the hook: it is not a key of the
file layer, `GET /api/v1/config`, the export or the import, a config file that
holds it is ignored, and `PUT /api/v1/config` with it answers `400`. Answers: `403` when off, `400` for ids that are not UUIDs,
`404` when this agent has no such mirror flow, `500` when the flow could not be
created again (the next pass retries while the mirror is wanted), `200` with
`old_inode`, `inode` and the `readers` (`pid`, `comm`) that mapped the old file.
Logs `mirror_rebuild_requested`; the next pass logs `mirror_rebuilt` with reason
`api` and counts it in `mirror_rebuilds_total`. It ignores
`MIRROR_CLEANUP_DRY_RUN`.

Exposed in `GET /api/v1/mirrors` (`flows[].health`), the metrics of §12.3,
the logs `mirror_stale`, `mirror_recovered`, `mirror_rebuilt` and the SSE event
`mirror`.

Platform contract: watch `mirror_stale` and `mirror_rebuilds_total` (or the SSE
`mirror` event). A receiver on this host reading a mirror that is `stale` gets
no new grains: the controller MAY switch it to another source after its own
hold-off. When `mirror_rebuilds_total` rises, every receiver on this host that
reads that flow SHOULD be activated again over IS-05 (same sender is fine) so the
media function opens the new file. `idle` with `origin_silent` is a source
without signal, not a mirror fault; `stale` with `peer_down` or `link_down`
means the path to the source is gone.

---

## 8. Peers

### 8.1 Discovery

Peers are the agents on other hosts. They are found by:

1. IS-04 Nodes with the service type from §6.3 (default), and/or
2. static entries in `PEERS` (config file), which take precedence.

Each peer is identified by `host_id`.

### 8.2 Peer link map (full mesh without switch)

In a switchless full mesh, every host pair has its own direct link and subnet.
The config therefore maps each peer to the local and remote fabric addresses:

```json
"peers": [
  { "host_id": "node-b", "control_url": "http://10.0.0.12:8095/api/v1",
    "local_fabric_addr": "192.168.12.1", "remote_fabric_addr": "192.168.12.2",
    "provider": "verbs" },
  { "host_id": "node-c",
    "local_fabric_addr": "192.168.13.1", "remote_fabric_addr": "192.168.13.3",
    "provider": "verbs" }
]
```

- `control_url` MAY be omitted when discovered via NMOS.
- If a peer has no static entry, the agent uses `DEFAULT_PROVIDER` and the
  fabric interface selected by `FABRIC_INTERFACE` (name or IP); this suits
  switched networks.
- Provider per link: `verbs` (RoCEv2) or `tcp`. `PROVIDER_FALLBACK=tcp` (default
  off) allows a link to fall back to `tcp` if `verbs` setup fails; the fallback
  is reported prominently.
- Link state: on every reconciliation tick the agent checks the local interface
  that holds the peer's `local_fabric_addr` (or `FABRIC_INTERFACE`): its carrier
  (`/sys/class/net/<netdev>/carrier`), or that some interface holds the address
  at all ("source interface unavailable"). While it is down the peer's link is
  `link_down` with the reason: a destination drops its targets and the source's
  connections for that peer and retries nothing; when the link is back it sets up
  fresh ones. The source's replications to that peer show `link_down` too, and its
  handshake answer says so, so the destination waits instead of rebuilding; the
  source sets nothing up for that peer meanwhile (since 1.3.1). One log line per
  change (`peer_link_down`, `peer_link_up`). There is no relay over a third host.
- Address list (since 1.3.1): libfabric's `verbs` provider lists its devices and
  their addresses once per process, at the first `fi_getinfo`, and answers from
  that list. An address that was missing then and comes back later (networkd
  after a link flap) is not in it. When a provider has no interface for a local
  fabric address that a local interface holds with carrier, the agent builds
  the list again (`fi_getinfo` with `FI_RESCAN`, at most once per 5 s, log
  `fabric_interfaces_rescanned`) and tries once more. The other replications go
  on.

### 8.3 Control API (agent-to-agent, also used by the UI)

Base path `/api/v1` on `WEB_PORT`. JSON. Unauthenticated by design (same
posture as the siblings: protected networks only).

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/info` | `host_id`, version, MXL/libfabric versions, providers available |
| GET | `/inventory` | local domains and flows (§5.3), monotonically increasing `revision`; each flow's `head_index` when the request is answered (not part of `revision`) |
| GET | `/events` | SSE stream: `inventory`, `replication`, `peer`, `mirror` (stale, recovered, rebuilt; §7.5) events |
| POST | `/replications` | destination asks source to add a target: `{flow_id, domain_id, dest_host_id, target_info}` → `201 {replication_id}` |
| DELETE | `/replications/{id}/targets/{dest_host_id}` | destination releases its target |
| GET | `/replications` | active replications (both roles) |
| GET | `/peers` | peers: control plane `up`, fabric `link_up`, `link_error`, `fabric_interface` |
| GET | `/mirrors` | local mirror domains: origin, flows (with `health`, §7.5), demand, readers, age, cleanup state |
| DELETE | `/mirrors/{domain_id}` | remove a mirror domain; 409 while another process maps it |
| POST | `/mirrors/{domain_id}/{flow_id}/rebuild` | test hook: rebuild one mirror flow (§7.5); 403 unless `MIRROR_ADMIN_REBUILD=true` |
| GET | `/demand` | local demand entries and the receivers behind them |
| GET | `/config` | effective configuration, where each key came from, `restart_required` |
| GET | `/config/env` | the same configuration as `KEY=value` lines |
| PUT | `/config` | patch the file layer (`WEB_ENABLE=false` rejects this) |
| GET | `/config/export` | one JSON document of the effective configuration |
| POST | `/config/import` | replace the file layer with that document. Environment still wins |

Inventory exchange: each agent polls peers' `/inventory` every
`PEER_POLL_INTERVAL_MS` (default 2000) and additionally reacts to `/events`.

### 8.4 Replication handshake (pull model, destination-driven)

1. Destination agent has a demand entry `(domain_id, flow_id)` resolved to a
   source peer and a mirror flow (creating it if needed).
2. Destination sets up a Fabrics **target** on the mirror flow's writer, bound to
   `local_fabric_addr` for that peer, and serialises its TargetInfo.
3. Destination `POST /replications` to the source with the serialised TargetInfo.
4. Source opens (or reuses) one MXL reader and one Fabrics **initiator** per
   origin flow, and adds the target. One initiator serves all destinations of a
   flow (fan-out).
5. Data flows. Destination drains completions (§9).
6. When demand disappears (receiver disabled, switched to another flow, or
   deleted) the destination waits `RELEASE_GRACE_MS` (default 2000, to absorb
   quick re-switches), then `DELETE`s its target at the source and tears down
   its target. The source removes the target from the initiator and destroys
   the initiator when no targets remain.
7. Both sides treat the handshake as idempotent: repeated POSTs for the same
   `(flow_id, dest_host_id)` return the existing replication; the destination
   re-POSTs on every reconciliation tick while its replication is not
   confirmed active by the source.

If the source restarts, the destination detects it (inventory `revision` reset
or `/info` boot id change) and repeats the handshake with a fresh target.

A link is `error` only while errors occur: a pass without a new error returns it
to `active` (a destination also needs new grains) and clears `last_error`; the
`errors` count keeps the history. A destination without a new grain for 5 s while
it is in `error` or the origin moved on after its last grain (a peer that hung,
a queue pair out of retries, a closed connection that reports nothing) tears its
target down and repeats the handshake with a fresh target, which the source puts
in place of the dead connection; `restarts` counts it. The source returns the
origin flow's head index in every `POST /replications` answer (`origin_head`);
a writer that holds the flow but writes nothing (no input signal) is not a dead
link. With a source that sends no `origin_head` (before 1.1.0) a writer on the
origin flow counts as moving on. The wait doubles with each rebuild in a row (5, 10, 20, 40 s). A successful
handshake clears the destination's last handshake error (`recv`, `http …`).

Since 1.3.0 the answer also carries the source's `state` for that replication
(`pending`, `active`, `error`, `link_down`) and its `error`, which the destination
shows as `last_error`. A source that cannot set up the replication answers 500
with the reason in `error`, which the destination shows as `http 500: <reason>`
(since 1.3.1; a bare `http 500` before). A source in `error` (connected, but no
transfer completed for 2 s) makes the destination rebuild after 1 s without a grain (1, 2, 4, 8 s in
a row); `link_down` makes it wait for the link. When MXL drops a source's target
because its connection shut down (closed by the destination, or after a failed
transfer: libfabric's `verbs` provider shuts an endpoint down on a completion
error), the source forgets it (`errors` + 1, `last_error`) and adds it again on
the destination's next request; the destination's target listens again after a
shutdown.

When a flow (domain id and flow id) is offered by more than one peer (a function
moved to another host and left its domain behind), the destination replicates
from one: a `live` origin before one with a writer before the rest, among equals
the current choice, else the lowest `host_id`. It logs `origin_conflict` when
the set of hosts or the choice changes and exports `origin_conflicts`.

---

## 9. Replication engine

- One Fabrics instance per provider in use. The Fabrics API is not assumed to
  be thread-safe: all calls on one instance are serialised on one dedicated
  thread per instance (the "fabric thread"), which runs progress for initiators
  and drains target completions.
- Initiator side: for each new grain index available in the origin reader
  (blocking wait with short timeout), transfer that grain to all targets.
  Continuous (audio) flows use MXL's sample transfer. One transfer carries
  every sample written since the last one, up to what the targets' bounce
  buffer entry holds (its size is in the target info), and starts one fabric
  thread pass (2 ms) after the last one completed: over `verbs` MXL's target
  posts the receive for the next immediate only when its agent drains the last
  transfer. 1.2.2 sent one sync batch per pass (48 samples from the ST 2110
  gateway): half of a 48 kHz flow arrived, 8 % over `verbs` on the platform.
  For continuous flows `replication_grains_total` counts transfers.
- Indexes the origin never wrote are passed over (since 1.3.1). A writer that is
  late and jumps to the current grain (mxl-replay's playout, FlowXer) leaves
  indexes unwritten; MXL marks them invalid with no valid slice when the writer
  opens the next grain. While a later grain exists, the source skips such an
  index, and one whose slot holds another index or is not complete, up to the
  next written grain; the mirror writer marks the skipped indexes invalid in
  turn. The head grain is always sent. `origin_gaps` in `/api/v1/replications`
  and `replication_origin_gaps_total` count them. Up to 1.3.0 each was sent as
  an empty grain: one transfer and one immediate more, back to back with the
  next grain, which over `verbs` meets "receiver not ready" (see pacing below).
- No transfer pacing: a grain goes out as one transfer. 1.1.0 had an optional
  pacing (slice batches spread over part of the grain duration); over `verbs`
  MXL's target keeps one receive posted for the immediate data that ends every
  transfer, so the batches ran into "receiver not ready" and retransmissions
  rose about 1000-fold on the platform's E810 mesh. Removed in 1.2.0;
  `TRANSFER_PACING` is ignored with a warning.
- Target side: drain completions with the batch/non-blocking read where
  available; never let the completion queue grow past its depth. Each received
  grain is committed to the mirror flow at the **same grain index** as the
  origin (indices, flow ids and ring geometry are preserved by the Fabrics API).
- Optional real-time scheduling for the fabric thread: `RT_PRIORITY` (0 = off,
  default), `CPU_AFFINITY` (list).
- Error handling per replication: on error, tear down only that replication,
  back off exponentially (`250 ms` → `30 s`, 10 s before 1.3.0), retry. Never
  affect other replications. A repeated failure is logged when it starts or its
  error changes, not per retry.
- Timing: grain indices are TAI-based. All hosts MUST be TAI-disciplined
  (PTP/chrony with correct kernel TAI offset). The agent checks `CLOCK_TAI`
  offset sanity at startup and exposes it as a metric; replicated readers on
  the destination will otherwise read the wrong grains or none.
- The agent measures replication lag as `origin head index − mirror head index`.
  Origin writers commit at the current TAI index, so the destination uses the
  flow's current index (`mxlGetCurrentIndex` at the flow rate) as the origin
  head; for continuous flows the lag counts transfer batches. It is 0 while no
  writer holds the origin flow, while the origin head the source reports has
  not moved for 2 s (a holder without a signal), and before the first grain.
  Because a batch of a continuous flow can be a single sample, the batch count
  says little about time (47 batches of 1 sample at 48 kHz are about 1 ms).
  The agent therefore also reports the lag in milliseconds: the same index
  distance at the flow's rate, i.e. samples ÷ sample rate for continuous flows
  and grains × grain period for discrete ones (`lag_ms`, rounded to whole
  microseconds; 0 in the same cases as the grain count).

---

## 10. Configuration

Same model as mxl-decklink: environment variables layered over an optional JSON
config file (`AGENT_CONFIG_FILE`). Precedence env > file > default. Env-set keys
are shown read-only in the UI. Invalid configuration exits 78. Changes to
global keys in the UI are flagged `restart_required`; peer entries and mirror
include/exclude lists apply at runtime.

| Key | Default | Meaning |
| --- | --- | --- |
| `HOST_ID` | hostname | fabrics peer identity; NMOS node id seed when `NMOS_SEED` is unset |
| `MXL_DOMAIN_SCAN_PATH` | `/Volumes/mxl` | parent of domain directories. Alias: `MXL_ROOT` |
| `STATE_DIR` | `/config` | directory for the configuration file this process writes |
| `SCAN_INTERVAL_MS` | 2000 | full re-scan interval |
| `MIRROR_MODE` | `eager` | `eager` or `on-demand` |
| `MIRROR_INCLUDE_DOMAINS` / `MIRROR_INCLUDE_FLOWS` | empty | allow lists; empty does not filter |
| `MIRROR_EXCLUDE_DOMAINS` / `MIRROR_EXCLUDE_FLOWS` | empty | deny lists |
| `MIRROR_GRACE_S` | 10 | keep mirror after origin disappears |
| `MIRROR_IDLE_GRACE_S` | 300 | drop a mirror domain after this long without demand, and a mirror flow after its origin flow has been missing this long (§7.4); `0` never |
| `MIRROR_CLEANUP_DRY_RUN` | false | mirror cleanup only logs |
| `MIRROR_STALE_MS` | 2000 | a mirror flow without a new grain this long while grains are due is `stale` (§7.5); `0` never |
| `MIRROR_ADMIN_REBUILD` | false | allow the rebuild test hook (§7.5); environment only (not in the config file, export, import or `PUT /config`), read at start |
| `TMPFS_RESERVE_MB` | 512 | minimum free space on the MXL filesystem |
| `MXL_CLEANUP_ON_EXIT` | false | remove own mirrors on SIGTERM. Alias: `CLEANUP_MIRRORS_ON_EXIT` |
| `DEFAULT_PROVIDER` | `verbs` | `verbs` or `tcp` |
| `PROVIDER_FALLBACK` | empty | `tcp` to allow fallback |
| `FABRIC_INTERFACE` | empty | default local fabric interface/IP |
| `FABRIC_PORT_BASE` | 23500 | first data port for targets |
| `FABRIC_PORT_COUNT` | 100 | size of the target port pool |
| `PEERS` | empty | peer link map (§8.2), file only or JSON in env |
| `PEER_POLL_INTERVAL_MS` | 2000 | inventory poll interval |
| `RELEASE_GRACE_MS` | 2000 | delay before releasing a target |
| `NMOS_ENABLE` | true | register own Node and observe registry |
| `NMOS_SEED` | empty | UUIDv5 name for the node id. Empty uses `HOST_ID` |
| `NMOS_LABEL` | `HOST_ID` | node label |
| `NMOS_TAGS` | `{}` | JSON object of tag name to array of strings, merged onto the node |
| `NMOS_DNS_SD` | false | `false` disables DNS-SD browse and mDNS advertisement |
| `NMOS_HOST_ADDRESS` | first non-loopback IPv4 | IP literal announced to other systems |
| `NMOS_REGISTRY_ADDRESS` / `NMOS_REGISTRY_PORT` | empty / 3210 | Registration API. Empty does not mean DNS-SD |
| `NMOS_QUERY_ADDRESS` / `NMOS_QUERY_PORT` | registry address / registry port + 1 | Query API |
| `NMOS_POLL_INTERVAL_MS` | 1000 | IS-05 `/active` reconciliation |
| `NMOS_PORT` | 3232 | own Node API (WebSocket listener on `NMOS_PORT+1`) |
| `LOCAL_NODE_IDS` / `LOCAL_NODE_HOSTNAMES` | empty | extra local-node rules (§6.1) |
| `LOCAL_NODE_CIDRS` | empty | comma-separated IPv4/IPv6 CIDRs; endpoint hosts inside them are local (§6.1) |
| `WEB_PORT` | 8095 | UI, REST, health, metrics |
| `WEB_ENABLE` | true | `false` hides the UI and `PUT /api/v1/config`. `/api/v1` stays up |
| `RT_PRIORITY` / `CPU_AFFINITY` | 0 / empty | fabric thread scheduling |
| `LOG_LEVEL` | `info` | structured JSON logs |
| `SHUTDOWN_TIMEOUT_S` | 10 | SIGTERM budget before exit 143 |
| `AGENT_CONFIG_FILE` | unset | optional JSON file. Saves use `$STATE_DIR/agent.json` when that directory is writable |

Combined `MIRROR_INCLUDE` / `MIRROR_EXCLUDE` are rejected. There are no secret settings.

Port defaults are chosen so they do not collide with mxl-decklink
(8080, 3212/3213) when all containers share host networking. The gateway's
defaults MUST be checked and documented in the README port table.

---

## 11. Requirements on media functions (documented, not implemented here)

The README MUST document what a media function needs to work with the agent:

1. Receivers resolve `mxl_domain_id` by scanning domains under the MXL root
   (mxl-decklink: `MXL_DOMAIN_SCAN_PATH`). Mirror domains are siblings of
   local domains.
2. If a flow is not (yet) present, the reader retries with backoff instead of
   failing permanently (mxl-decklink already does this on `FLOW_NOT_FOUND`).
3. Readers tolerate a flow that exists but has no new grains yet.
4. Media functions do not need `MXL_ENABLE_FABRICS_OFI`.
5. Readers on a destination host should read with a small latency offset
   (≥ replication lag, see metrics) to avoid "too early" reads.

The same list SHOULD be added to the mxl-st2110-gateway spec.

---

## 12. Web, operations and monitoring

### 12.1 HTTP endpoints on `WEB_PORT`

- `/livez` is 200 while the process is up.
- `/readyz` is 200 when the scanner is running and, if a registry is configured
  (`NMOS_REGISTRY_ADDRESS` set or `NMOS_DNS_SD=true`), the node is registered.
- `/metrics` — Prometheus text, prefix `mxl_fabrics_agent_`.
- `/api/v1/...` — §8.3, plus `GET /config/export` and `POST /config/import`.
  Export is the effective configuration. Import replaces the file layer.
  Environment variables still win. No field is a secret.
- `/` — admin UI (when `WEB_ENABLE=true`).

### 12.2 Admin UI (Vue SPA)

Tabs:

- **Overview**: host id, versions, providers, TAI offset, registry status,
  peers (up/down, link, provider), counts.
- **Domains & Flows**: local domains and flows (active/inactive), mirror
  domains and mirror flows with state (`idle`, `replicating`, `orphaned`,
  `insufficient_space`, `peer_down`, `conflict`).
- **Replications**: per replication role, peer, provider, grains/s, Gbit/s,
  lag (grains and ms), errors, restarts, last error.
- **Demand**: receivers on this host (node, label, active `mxl_domain_id` /
  `mxl_flow_id`), resolved source host, state (`local`, `replicating`,
  `unresolved`, `stale_reference`).
- **Peers**: peer link map editing, test-connect button (sets up a dummy
  target/initiator pair and reports result).
- **Settings**: effective configuration, import/export of the config file,
  `KEY=value` export (as in mxl-decklink).

### 12.3 Prometheus metrics (prefix `mxl_fabrics_agent_`)

| Metric | Type | Labels |
| --- | --- | --- |
| `info` | gauge (1) | host_id, version, mxl_version, libfabric_version |
| `domains` | gauge | kind (local/mirror/conflict) |
| `flows` | gauge | kind (origin/mirror), state |
| `tmpfs_free_bytes` / `tmpfs_size_bytes` | gauge | — |
| `peers_up` | gauge | — |
| `peer_up` | gauge | peer, interface; 1 while the control plane answers and the local fabric link has carrier (control plane only and no `interface` before 1.3.0) |
| `peer_rdma_retransmits_total` | counter | peer, device; `RetransSegs` of the RDMA device on the fabric interface (irdma), absent without one |
| `origin_conflicts` | gauge | —; flows offered by more than one peer |
| `demand_entries` | gauge | state |
| `replications_active` | gauge | role (source/destination), provider |
| `replication_state` | gauge (1) | flow_id, peer, role, state (`pending`, `active`, `error`, `link_down`, `idle`) |
| `replication_grains_total` | counter | flow_id, peer, role |
| `replication_bytes_total` | counter | flow_id, peer, role |
| `replication_errors_total` | counter | flow_id, peer, role (no `kind`: errors are counted, not classified) |
| `replication_restarts_total` | counter | flow_id, peer |
| `replication_origin_gaps_total` | counter | flow_id, peer; source: origin indexes never written, passed over (since 1.3.1) |
| `replication_lag_grains` | gauge | flow_id, peer; transfer batches for continuous flows |
| `replication_lag_ms` | gauge | flow_id, peer; the lag in milliseconds at the flow's rate |
| `grain_transfer_seconds` | histogram | provider; source grains, transfer to completion |
| `setup_seconds` | histogram | phase (target_setup, handshake, first_grain) (not implemented yet) |
| `nmos_registry_up` | gauge | — |
| `nmos_poll_errors_total` | counter | — (not implemented yet) |
| `tai_offset_seconds` | gauge | — |
| `completion_queue_depth` | gauge | flow_id |
| `mirror_stale` | gauge (0/1) | domain_id, flow_id, peer; 1 while the destination mirror flow is `stale` (§7.5) |
| `mirror_state` | gauge (1) | domain_id, flow_id, peer, state (`live`, `pending`, `stale`, `idle`) |
| `mirror_last_grain_age_seconds` | gauge | domain_id, flow_id, peer; age of the mirror's newest grain, absent before the first |
| `mirror_rebuilds_total` | counter | domain_id, flow_id, peer; the mirror's data file was created again |
| `mirrors_stale` | gauge | —; mirror flows that are `stale` |
| `mirror_flows_removed_total` | counter | reason; single mirror flows removed: `origin_missing` (§7.4, since 1.6.0), `origin_gone`, `no_demand`, `excluded` |
| `mirror_flows_refused_total` | counter | reason; a removal refused because another process maps the flow or the reader check is incomplete, counted once per change |

Label cardinality: `flow_id` labels are allowed (PoC scale); a config switch
`METRICS_PER_FLOW=false` drops per-flow labels (the `mirror_*` series then
reduce to `mirrors_stale` and an unlabelled `mirror_rebuilds_total`).

### 12.4 Grafana dashboard

`deploy/grafana/mxl-fabrics-agent.json`: importable dashboard with a
`host_id` variable, panels for peers, replications, throughput per flow, lag,
errors/restarts, setup time, tmpfs usage, TAI offset. Plus
`deploy/prometheus/prometheus.yml` example scrape config for all hosts.

### 12.5 Logging

Structured JSON logs, one event per line, with `replication_id`, `flow_id`,
`peer` fields where relevant. Mirror health (§7.5): `mirror_stale` (warn;
`domain_id`, `flow_id`, `peer`, `reason`, `last_grain_age_ms`, `stale_after_ms`),
`mirror_recovered` (info; the new `state` and `reason`, `stale_ms`),
`mirror_rebuilt` (warn; `reason`, `rebuilds`, `old_inode`, `inode`). Single-flow
cleanup (§7.4): `mirror_flow_origin_missing` (info; `domain_id`, `flow_id`,
`peer`, `grace_s`), `mirror_flow_removed` (info; `domain_id`, `flow_id`,
`path`, `reason`, `missing_s`), `mirror_flow_kept_reader` (warn; `refused`),
`mirror_flow_would_remove` (info; dry run), `mirror_flow_remove_failed` (warn).
Rebuilds (§7.4, §7.5): `mirror_origin_changed` (warn; `domain_id`, `flow_id`,
`peer`), `mirror_rebuild_requested` (warn; `domain_id`, `flow_id`, `old_inode`,
`inode`, `readers`, `error`).

### 12.6 Exit codes (aligned with siblings)

| Code | Meaning |
| --- | --- |
| 0 | the process returned from `main` without a termination signal |
| 75 | a listen port could not be bound, or no fabric provider is usable (`EX_TEMPFAIL`) |
| 78 | invalid configuration, or the MXL scan path is missing (`EX_CONFIG`) |
| 143 | SIGTERM or SIGINT completed, or `SHUTDOWN_TIMEOUT_S` elapsed |

---

## 13. Container and host requirements

- Base image and multi-stage build like mxl-decklink (build MXL with Fabrics,
  libfabric, nmos-cpp in builder stages; slim runtime stage).
- Runtime requirements:
  - host networking (RDMA and peer fabric addresses; DNS-SD is off unless `NMOS_DNS_SD=true`);
  - `/dev/infiniband` devices (`uverbs*`, `rdma_cm`);
  - capability `IPC_LOCK` and `memlock` ulimit unlimited (memory registration);
  - MXL root bind-mounted read-write;
  - runs as the uid/gid owning the MXL root (e.g. `1000:1000`);
  - `/run/dbus` and `/run/avahi-daemon` only when `NMOS_DNS_SD=true`.
- Host prerequisites (documented in README): rdma-core, irdma loaded with
  RoCEv2 enabled on the E810 ports used for the mesh (verify the irdma RoCE
  mode setting), IP addresses on the direct links, MTU consistent per link,
  PTP/TAI discipline, tmpfs sized for local flows plus mirrors (sizing table
  in README: bytes per grain × ring depth × number of mirrored flows).

---

## 14. CI and releases

Same as the siblings:

- GitHub Actions: build, unit tests, integration tests (§15.2, `tcp` provider),
  build and push to GHCR `ghcr.io/leeo86/<repo-name>`.
- Tags: `vX.Y.Z` → immutable `X.Y.Z`, `X.Y`, `X`, plus a GitHub Release.
  `main` → moving `nightly-dev`. Every build → `git-<sha7>`.
  `latest` is not published. OCI labels include `org.opencontainers.image.source`,
  `.revision`, `.licenses`, and `io.dmf.mxl.revision` (the MXL commit).
  The runtime image runs as uid 1000. Host networking needs `IPC_LOCK` for RDMA,
  not root.

---

## 15. Testing

### 15.1 Unit tests

Inventory parsing and classification, demand derivation from IS-05 `/active`
fixtures, local-node matching, mirror lifecycle state machine, config
precedence and validation, handshake idempotency.

### 15.2 Integration tests without RDMA hardware

Single machine, `tcp` provider over loopback, two simulated hosts:

- two MXL roots (`/dev/shm/mxl-a`, `/dev/shm/mxl-b`), two agent instances with
  different `HOST_ID`, ports, and static `PEERS`;
- a test writer (small tool in `tests/`, or mxl-decklink with
  `MXL_DECKLINK_BACKEND=mock`) on root A;
- an nmos-cpp registry and a fake receiver node (or mxl-decklink mock output
  channel) on root B;
- test steps: IS-05 PATCH of the receiver → mirror exists before activation
  (eager) → grains arrive with identical indices → disable receiver → target
  released after grace → kill source agent → `peer_down` → restart →
  replication resumes → format change at source → `stale_reference` reported →
  origin writer re-creates its flow → replication resumes → destination agent
  stopped → the source logs a few lines, not one per pass → destination back →
  replication resumes.
- `tests/integration/shutdown.sh`: start, `/readyz` after registration, SIGTERM,
  exit 143, the registry records DELETE, and only this agent's mirror directory
  is removed.
- `tests/integration/flow_cleanup.sh` (as root, for the reader check): three
  flows in one mirror domain, two origins end → dry run logs and keeps both →
  the unread one is removed → the one a reader maps stays until the reader
  exits → the live flow and the domain stay.
- `tests/integration/rebuild.sh`: a reader of the mirror that re-opens on
  `MXL_ERR_FLOW_INVALID` (`mxl-pattern-writer follow`) → the rebuild hook answers
  403 where it is off, 400 for a bad id, 404 for an unknown flow → a rebuild
  through the hook gives a new inode, `mirror_rebuilt` (`api`) and
  `mirror_rebuilds_total` 1, and the reader reads again → the origin writer
  exits and comes back at 25 instead of 50 grains/s under the same flow id →
  the mirror is rebuilt (`origin_config_changed`) with the new rate and the
  reader reads again.

### 15.3 NMOS checks

The agent's own Node passes the IS-04 node tests of the AMWA NMOS Testing tool
(no senders/receivers). Media functions are tested separately.

### 15.4 Pre-go-live checks (hardware)

- `verbs` on E810-XXVDA2 over the direct-link full mesh, 3 hosts, 1080p v210
  and audio, sustained 24 h without drops.
- Fan-out: one origin to two destinations.
- Concurrent setup of ≥ 10 mirrors (endpoint-id collision issue).
- Completion queue under load at 1080p50/2160p50.
- Empirical tmpfs and CPU sizing.

---

## 16. Demo deployments

### 16.1 Docker Compose (`docker/`)

1. `docker-compose.demo.yaml` — **single machine, no RDMA**: nmos-cpp registry,
   two agents (host A / host B simulated with separate MXL roots, `tcp`
   provider), mxl-decklink mock input on A, mxl-decklink mock output on B,
   Prometheus and Grafana with the dashboard provisioned. README contains the
   step-by-step demo including the IS-05 PATCH via `curl` (and optionally a
   web controller).
2. `docker-compose.host.yaml` — **one file per real host**: agent with host
   networking, devices, `IPC_LOCK`, memlock, MXL root mount, config file mount.
   README shows the three-host full-mesh example with a matching `peers` block
   per host.

### 16.2 Kubernetes (`deploy/`)

- `mxl-fabrics-agent.yaml`: DaemonSet (one agent per node), `hostNetwork: true`,
  `dnsPolicy: ClusterFirstWithHostNet`, `IPC_LOCK`, hostPath for the MXL root,
  ConfigMap for the config file with per-node peer maps (selected by
  `HOST_ID` = node name via the downward API), readiness/liveness probes,
  Prometheus scrape annotations.
- RDMA device access via the generic device plugin (as in
  mxl-decklink's `deploy/generic-device-plugin.yaml`) exposing
  `/dev/infiniband`.
- `monitoring/`: example ServiceMonitor and the Grafana dashboard as ConfigMap.

---

## 17. Implementation order (suggested)

1. Skeleton, config, web/ops server, `/metrics`, CI, Dockerfile.
2. Domain scanner + inventory API.
3. Peer manager with static peers; inventory exchange.
4. Mirror manager (eager mode) — verifiable without Fabrics.
5. Replication engine with `tcp`; handshake; integration test §15.2.
6. NMOS observer and own Node; demand-driven replication.
7. Admin UI, Grafana dashboard, demo compose, Kubernetes manifests.
8. `verbs` validation on hardware (§15.4).

## 18. Open points

- Final repository name.
- Whether the agent should later also watch IS-05 `/staged` with scheduled
  activations to pre-start data transfer (not in v1).
- Whether mirror markers should be proposed upstream to MXL as a standard
  domain attribute.
