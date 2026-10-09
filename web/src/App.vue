<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";

const tab = ref("overview");
const info = ref({});
const status = ref({});
const inventory = ref({ domains: [] });
const mirrors = ref([]);
const replications = ref([]);
const demand = ref({ receivers: [] });
const peers = ref([]);
const configDoc = ref({ config: {}, origin: {}, restart_required: false });
const message = ref("");
let timer = 0;

async function get(path) {
  const response = await fetch(path);
  if (!response.ok) throw new Error(path + " " + response.status);
  const type = response.headers.get("content-type") || "";
  return type.includes("json") ? response.json() : response.text();
}

async function refresh() {
  try {
    const [i, s, inv, mir, rep, dem, pee, cfg] = await Promise.all([
      get("/api/v1/info"),
      get("/statusz"),
      get("/api/v1/inventory"),
      get("/api/v1/mirrors"),
      get("/api/v1/replications"),
      get("/api/v1/demand"),
      get("/api/v1/peers"),
      get("/api/v1/config"),
    ]);
    info.value = i;
    status.value = s;
    inventory.value = inv;
    mirrors.value = mir;
    replications.value = rep;
    demand.value = dem;
    peers.value = pee;
    configDoc.value = cfg;
  } catch (err) {
    message.value = String(err);
  }
}

function pill(state) {
  if (state === "replicating" || state === "active" || state === "local") return "pill ok";
  if (state === "idle" || state === "pending" || state === "peer_down") return "pill warn";
  if (!state) return "pill";
  return "pill bad";
}

const configText = computed({
  get: () => JSON.stringify(configDoc.value.config || {}, null, 2),
  set: () => {},
});
const draft = ref("");

async function saveConfig() {
  const response = await fetch("/api/v1/config", {
    method: "PUT",
    headers: { "content-type": "application/json" },
    body: draft.value || configText.value,
  });
  const body = await response.json();
  message.value = response.ok ? JSON.stringify(body) : body.error || "save failed";
  await refresh();
}

async function exportEnv() {
  const text = await get("/api/v1/config/env");
  const blob = new Blob([text], { type: "text/plain" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = "mxl-fabrics-agent.env";
  a.click();
  URL.revokeObjectURL(url);
}

async function testPeer(id) {
  message.value = "testing " + id;
  const response = await fetch("/api/v1/peers/" + encodeURIComponent(id) + "/test", { method: "POST" });
  message.value = JSON.stringify(await response.json());
}

onMounted(() => {
  refresh();
  timer = setInterval(refresh, 1000);
});
onUnmounted(() => clearInterval(timer));
</script>

<template>
  <header>
    <h1>mxl-fabrics-agent</h1>
    <div class="muted">{{ info.host_id }} · {{ info.version }}</div>
  </header>
  <nav>
    <button v-for="name in ['overview', 'domains', 'replications', 'demand', 'peers', 'settings']" :key="name" :class="{ active: tab === name }" @click="tab = name">
      {{ name }}
    </button>
  </nav>
  <main>
    <p v-if="message" class="muted">{{ message }}</p>
    <section v-if="tab === 'overview'" class="grid">
      <div class="card"><div class="k">Host</div><div class="v">{{ info.host_id }}</div></div>
      <div class="card"><div class="k">Boot</div><div class="v" style="font-size:13px">{{ info.boot_id }}</div></div>
      <div class="card"><div class="k">MXL</div><div class="v" style="font-size:14px">{{ info.mxl_version }}</div></div>
      <div class="card"><div class="k">TAI offset</div><div class="v">{{ info.tai_offset_seconds }}s</div></div>
      <div class="card"><div class="k">Registry</div><div class="v">{{ status.nmos ? "up" : "down" }}</div></div>
      <div class="card"><div class="k">Peers up</div><div class="v">{{ peers.filter(p => p.up && p.link_up !== false).length }} / {{ peers.length }}</div></div>
      <div class="card"><div class="k">Local domains</div><div class="v">{{ (inventory.domains || []).length }}</div></div>
      <div class="card"><div class="k">Replications</div><div class="v">{{ replications.length }}</div></div>
    </section>

    <section v-else-if="tab === 'domains'">
      <h2>Local</h2>
      <table>
        <thead><tr><th>Domain</th><th>Flow</th><th>Format</th><th>Active</th></tr></thead>
        <tbody>
          <template v-for="domain in inventory.domains || []" :key="domain.domain_id">
            <tr v-for="flow in domain.flows" :key="domain.domain_id + flow.flow_id">
              <td>{{ domain.domain_id }}</td>
              <td>{{ flow.flow_id }}<div class="muted">{{ flow.media_type }}</div></td>
              <td>{{ flow.format }}</td>
              <td><span :class="flow.active ? 'pill ok' : 'pill'">{{ flow.active ? "active" : "inactive" }}</span></td>
            </tr>
          </template>
        </tbody>
      </table>
      <h2>Mirrors</h2>
      <table>
        <thead><tr><th>Domain</th><th>Source</th><th>Flow</th><th>State</th></tr></thead>
        <tbody>
          <template v-for="mirror in mirrors" :key="mirror.path">
            <tr v-for="flow in mirror.flows" :key="mirror.path + flow.flow_id">
              <td>{{ mirror.domain_id }}</td>
              <td>{{ mirror.source_host_id }}</td>
              <td>{{ flow.flow_id }}</td>
              <td><span :class="pill(mirror.state)">{{ mirror.state }}</span></td>
            </tr>
          </template>
        </tbody>
      </table>
    </section>

    <section v-else-if="tab === 'replications'">
      <table>
        <thead><tr><th>Role</th><th>Peer</th><th>Provider</th><th>Flow</th><th>State</th><th>Grains</th><th>Lag</th><th>Errors</th><th>Last error</th></tr></thead>
        <tbody>
          <tr v-for="row in replications" :key="row.replication_id + row.role + row.peer">
            <td>{{ row.role }}</td>
            <td>{{ row.peer }}</td>
            <td>{{ row.provider }}<span v-if="row.fallback" class="pill warn">tcp fallback</span></td>
            <td>{{ row.flow_id }}</td>
            <td><span :class="pill(row.state)">{{ row.state }}</span></td>
            <td>{{ row.grains }}</td>
            <td>{{ row.lag_grains }}</td>
            <td>{{ row.errors }} / {{ row.restarts }}</td>
            <td class="muted">{{ row.last_error }}</td>
          </tr>
        </tbody>
      </table>
    </section>

    <section v-else-if="tab === 'demand'">
      <table>
        <thead><tr><th>Node</th><th>Receiver</th><th>Domain</th><th>Flow</th><th>Source</th><th>State</th></tr></thead>
        <tbody>
          <tr v-for="rx in demand.receivers || []" :key="rx.id">
            <td>{{ rx.node_label || rx.node_id }}</td>
            <td>{{ rx.label }}</td>
            <td>{{ rx.mxl_domain_id }}</td>
            <td>{{ rx.mxl_flow_id }}</td>
            <td>{{ rx.source_host }}</td>
            <td><span :class="pill(rx.state)">{{ rx.state }}</span></td>
          </tr>
        </tbody>
      </table>
    </section>

    <section v-else-if="tab === 'peers'">
      <table>
        <thead><tr><th>Host</th><th>Control</th><th>Link</th><th>Provider</th><th>State</th><th></th></tr></thead>
        <tbody>
          <tr v-for="peer in peers" :key="peer.host_id">
            <td>{{ peer.host_id }}</td>
            <td>{{ peer.control_url }}</td>
            <td>{{ peer.local_fabric_addr }} → {{ peer.remote_fabric_addr }}<div v-if="peer.link_up === false"><span class="pill bad">link down</span> <span class="muted">{{ peer.link_error }}</span></div></td>
            <td>{{ peer.provider }}</td>
            <td><span :class="peer.up ? 'pill ok' : 'pill bad'">{{ peer.up ? "up" : "down" }}</span><div class="muted">{{ peer.error }}</div></td>
            <td><button @click="testPeer(peer.host_id)">Test connect</button></td>
          </tr>
        </tbody>
      </table>
      <p class="muted">Peer entries and mirror filters are edited in Settings and apply without a restart. Other keys set restart_required.</p>
    </section>

    <section v-else>
      <div class="row">
        <span v-if="configDoc.restart_required" class="pill warn">restart required</span>
        <button class="primary" @click="saveConfig">Save file layer</button>
        <button @click="exportEnv">Export KEY=value</button>
      </div>
      <textarea v-model="draft" :placeholder="configText"></textarea>
    </section>
  </main>
</template>
