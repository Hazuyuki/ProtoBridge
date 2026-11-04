"use strict";

const $ = (id) => document.getElementById(id);
const esc = (value) =>
  String(value ?? "").replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
const number = (value) =>
  value == null ? "—" : Number(value).toLocaleString("en-US");
const terminal = new Set([
  "completed",
  "failed",
  "incomplete",
  "cancelled",
  "timed_out",
  "interrupted",
]);
const statusLabels = {
  queued: "Queued",
  running: "Running",
  cancelling: "Cancelling",
  completed: "Completed",
  incomplete: "Incomplete",
  failed: "Failed",
  cancelled: "Cancelled",
  timed_out: "Timed out",
  interrupted: "Interrupted",
};
const state = {
  catalog: null,
  scene: "collective",
  config: null,
  baseStack: {},
  baseOp: {},
  controls: null,
  ready: false,
  engine: null,
  preview: null,
  previewVersion: 0,
  selected: null,
  runs: [],
  compared: new Set(),
  tab: "overview",
  log: "",
  logCursor: 0,
  historyKey: "",
  traceLoaded: null,
  submitting: false,
  formMessage: "",
  toastMessage: "",
  connectionMessage: null,
};
let previewTimer,
  toastTimer,
  polling = false;

class ApiError extends Error {
  constructor(detail, status) {
    super(typeof detail === "string" ? detail : `HTTP ${status}`);
    this.detail = detail;
    this.status = status;
  }
  localized() {
    return Array.isArray(this.detail)
      ? this.detail
          .map(
            (issue) =>
              `${issue.loc.slice(1).join(".")}: ${validationMessage(issue)}`,
          )
          .join("; ")
      : t(this.detail) ||
          t("Request failed ({value0})", { value0: this.status });
  }
}
function messageText(message) {
  return message instanceof ApiError
    ? message.localized()
    : t(message instanceof Error ? message.message : message);
}
async function api(path, options = {}) {
  const response = await fetch(path, {
    cache: "no-store",
    ...options,
    headers: { "Content-Type": "application/json", ...options.headers },
  });
  let data;
  try {
    data = await response.json();
  } catch {
    throw new Error(
      t("Cannot read the server response. Check that the service is running."),
    );
  }
  if (!response.ok) {
    throw new ApiError(data.detail, response.status);
  }
  return data;
}
function displayRunName(run) {
  return run.name_is_default ? t(run.name) : run.name;
}
function validationMessage(issue) {
  const types = {
    int_type: "Expected an integer.",
    int_parsing: "Expected an integer.",
    float_type: "Expected a number.",
    float_parsing: "Expected a number.",
    dict_type: "Expected an object.",
    extra_forbidden: "Unknown field.",
    missing: "This field is required.",
    string_pattern_mismatch: "The value does not match the required format.",
  };
  if (types[issue.type]) return t(types[issue.type]);
  const bounds = {
    greater_than_equal: ["Must be at least {limit}.", "ge"],
    less_than_equal: ["Must be at most {limit}.", "le"],
    string_too_short: [
      "Must contain at least {limit} characters.",
      "min_length",
    ],
    string_too_long: ["Must contain at most {limit} characters.", "max_length"],
  };
  const bound = bounds[issue.type];
  return bound ? t(bound[0], { limit: issue.ctx[bound[1]] }) : t(issue.msg);
}
function notify(message) {
  state.toastMessage = message;
  $("toast").textContent = messageText(message);
  $("toast").hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    $("toast").hidden = true;
  }, 3500);
}
function formError(message = "") {
  state.formMessage = message;
  $("form-error").textContent = messageText(message);
  $("form-error").hidden = !message;
}
function bytesLabel(bytes) {
  if (bytes >= 1073741824) return `${+(bytes / 1073741824).toFixed(3)} GiB`;
  if (bytes >= 1048576) return `${+(bytes / 1048576).toFixed(3)} MiB`;
  if (bytes >= 1024) return `${+(bytes / 1024).toFixed(3)} KiB`;
  return `${bytes} B`;
}
function durationLabel(ns) {
  return ns == null
    ? "—"
    : `${(Number(ns) / 1000).toLocaleString("en-US", { maximumFractionDigits: 3 })} µs`;
}
function badge(status) {
  return `<span class="status-badge ${esc(status)}">${esc(t(statusLabels[status] || status))}</span>`;
}

function setScene(scene, changeConfig = true) {
  state.scene = scene;
  document.querySelectorAll("[data-scene]").forEach((button) => {
    const active = button.dataset.scene === scene;
    button.classList.toggle("active", active);
    button.setAttribute("aria-pressed", String(active));
  });
  const multi = scene === "multinode";
  $("single-node-fields").hidden = multi;
  $("multi-node-fields").hidden = !multi;
  $("inter-node-controls").hidden = !multi;
  $("num-gpus").disabled = multi;
  $("num-nodes").disabled = !multi;
  $("gpus-per-node").disabled = !multi;
  const configs = state.catalog.configs.filter(
    (config) => config.kind === scene,
  );
  $("config-select").innerHTML = configs
    .map(
      (config) =>
        `<option value="${esc(config.id)}">${esc(t(config.title))}</option>`,
    )
    .join("");
  $("config-count").textContent = t("{value0} templates", {
    value0: configs.length,
  });
  if (changeConfig && configs.length) loadConfig(configs[0].id);
}
function setSize(bytes) {
  const unit =
    bytes >= 1048576 && bytes % 1048576 === 0
      ? 1048576
      : bytes >= 1024 && bytes % 1024 === 0
        ? 1024
        : 1;
  $("data-unit").value = String(unit);
  $("data-size").value = bytes / unit;
}
function loadConfig(id, request = null, copyName = true) {
  const config = state.catalog.configs.find((item) => item.id === id);
  if (!config) return;
  state.config = config;
  if (state.scene !== config.kind) setScene(config.kind, false);
  $("config-select").value = id;
  $("config-description").textContent = t(config.description);
  $("config-editor").value = request?.config_text ?? config.text;
  $("profile-editor").value = request?.profile_text ?? config.profile_text;
  const defaults = { ...config.defaults, ...request };
  state.baseStack = { ...(defaults.stack_overrides || {}) };
  state.baseOp = { ...(defaults.op_overrides || {}) };
  $("num-gpus").value = defaults.num_gpus ?? 8;
  $("num-nodes").value = defaults.num_nodes > 1 ? defaults.num_nodes : 2;
  $("gpus-per-node").value = defaults.gpus_per_node ?? 4;
  $("seed").value = defaults.seed ?? 1;
  $("timeout").value = defaults.timeout ?? 120;
  $("run-name").value = request
    ? `${request.name || t(config.title)}${copyName ? t(" · Copy") : ""}`.slice(
        0,
        80,
      )
    : "";
  $("cli-json").value = JSON.stringify(defaults.cli_overrides || {}, null, 2);
  setSize(defaults.data_size ?? 1048576);
  $("bandwidth").value = state.baseStack.bandwidthGbps ?? "";
  $("latency").value = state.baseStack.latencyNs ?? "";
  $("inter-bandwidth").value = state.baseStack.interNodeBandwidthGbps ?? "";
  $("inter-latency").value = state.baseStack.interNodeLatencyNs ?? "";
  const fabric =
    state.baseStack.interNodeFabricType ||
    config.text.match(/^interNodeFabricType\s*=\s*(\w+)/m)?.[1] ||
    "RoCE";
  $("inter-fabric").value = fabric;
  $("inter-topology").value =
    state.baseStack.interNodeTopology ||
    config.text.match(/^interNodeTopology\s*=\s*(\w+)/m)?.[1] ||
    "fullmesh";
  state.controls.reset(defaults);
  $("cli-json").value = JSON.stringify(
    state.controls.extraCli(defaults.cli_overrides),
    null,
    2,
  );
  formError();
  updateLocalLabels();
  refreshPreview();
}
function updateLocalLabels() {
  const bytes = Math.round(
    Number($("data-size").value) * Number($("data-unit").value),
  );
  const ranks =
    state.scene === "multinode"
      ? Number($("num-nodes").value) * Number($("gpus-per-node").value)
      : Number($("num-gpus").value);
  $("data-bytes").textContent = `${number(bytes)} bytes`;
  $("run-summary").textContent = state.preview?.summary.stencil
    ? `${ranks} ranks · dataSize=${bytesLabel(bytes)}`
    : `${ranks} ranks · ${bytesLabel(bytes)} / rank`;
  document
    .querySelectorAll("[data-gpus]")
    .forEach((button) =>
      button.classList.toggle(
        "active",
        button.dataset.gpus === $("num-gpus").value,
      ),
    );
}
function requestBody() {
  const cli = JSON.parse($("cli-json").value || "{}");
  if (!cli || typeof cli !== "object" || Array.isArray(cli))
    throw new Error(t("Extra CLI flags must be a JSON object."));
  const stack = { ...state.baseStack };
  for (const [id, key] of [
    ["bandwidth", "bandwidthGbps"],
    ["latency", "latencyNs"],
    ["inter-bandwidth", "interNodeBandwidthGbps"],
    ["inter-latency", "interNodeLatencyNs"],
  ]) {
    if ($(id).value !== "") stack[key] = Number($(id).value);
    else delete stack[key];
  }
  if (state.scene === "multinode") {
    stack.interNodeFabricType = $("inter-fabric").value;
    stack.interNodeTopology = $("inter-topology").value;
  }
  return state.controls.apply({
    name: $("run-name").value.trim(),
    config_id: state.config.id,
    config_text: $("config-editor").value,
    profile_text: $("profile-editor").value,
    num_gpus: Number($("num-gpus").value),
    num_nodes: state.scene === "multinode" ? Number($("num-nodes").value) : 1,
    gpus_per_node: Number($("gpus-per-node").value),
    data_size: Math.round(
      Number($("data-size").value) * Number($("data-unit").value),
    ),
    seed: Number($("seed").value),
    timeout: Number($("timeout").value),
    stack_overrides: stack,
    op_overrides: { ...state.baseOp },
    cli_overrides: cli,
  });
}
async function refreshPreview() {
  if (!state.config) return;
  const version = ++state.previewVersion;
  try {
    const result = await api("/api/preview", {
      method: "POST",
      body: JSON.stringify(requestBody()),
    });
    if (version !== state.previewVersion) return;
    state.preview = result;
    state.controls.update(result.summary);
    if (
      state.scene !== "multinode" &&
      result.summary.ranks !== Number($("num-gpus").value)
    ) {
      $("num-gpus").value = result.summary.ranks;
    }
    document.querySelector('label[for="data-size"]').textContent = result
      .summary.stencil
      ? t("dataSize input")
      : t("Application bytes per rank");
    updateLocalLabels();
    formError();
    renderTopology(result.summary);
    $("effective-bandwidth").textContent =
      `${result.summary.bandwidth_gbps} Gbps`;
    $("effective-latency").textContent = `${result.summary.latency_ns} ns`;
    $("effective-profile").textContent = result.summary.profile;
    $("bandwidth").placeholder = result.summary.bandwidth_gbps;
    $("latency").placeholder = result.summary.latency_ns;
    $("inter-bandwidth").placeholder = result.summary.inter_node_bandwidth_gbps;
    $("inter-latency").placeholder = result.summary.inter_node_latency_ns;
    $("preview-notices").textContent = result.notices
      .map((message) => t(message))
      .join(" ");
    $("preview-notices").hidden = !result.notices.length;
    $("stencil-hint").hidden = !result.summary.stencil;
    $("stencil-hint").textContent =
      t(result.notices[0]) ||
      t(
        "The transfer.*.bytes expressions determine the actual transfer sizes.",
      );
    $("run-button").disabled = !state.ready || state.submitting;
    return true;
  } catch (error) {
    if (version === state.previewVersion) {
      $("run-button").disabled = true;
      formError(
        error instanceof SyntaxError
          ? t("Invalid JSON in Extra CLI flags.")
          : error,
      );
    }
    return false;
  }
}
function schedulePreview() {
  state.controls?.sync();
  updateLocalLabels();
  clearTimeout(previewTimer);
  previewTimer = setTimeout(refreshPreview, 350);
}

function renderTopology(summary) {
  const width = 700,
    height = 270,
    count = summary.ranks;
  let svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" aria-hidden="true"><defs><marker id="network-arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="4" markerHeight="4" orient="auto-start-reverse"><path d="M0 0 10 5 0 10Z" fill="#7198ed"/></marker></defs>`;
  const gpu = (x, y, label, active = true) =>
    `<g><rect x="${x - 27}" y="${y - 15}" width="54" height="30" rx="6" fill="${active ? "#f4f8ff" : "#fbfcfe"}" stroke="${active ? "#91b0f2" : "#d8e1ee"}"/><rect x="${x - 19}" y="${y - 5}" width="10" height="10" rx="2" fill="none" stroke="${active ? "#6a92e7" : "#b7c5d8"}"/><text x="${x + 6}" y="${y + 4}" text-anchor="middle" font-size="9" fill="#5a7294">${esc(label)}</text></g>`;
  if (summary.num_nodes > 1) {
    const shown = Math.min(summary.num_nodes, 4);
    const boxWidth = shown === 2 ? 240 : shown === 3 ? 180 : 145;
    const gap = 38,
      total = shown * boxWidth + (shown - 1) * gap,
      start = (width - total) / 2;
    for (let n = 0; n < shown; n++) {
      const x = start + n * (boxWidth + gap),
        center = x + boxWidth / 2;
      svg += `<rect x="${x}" y="32" width="${boxWidth}" height="201" rx="10" fill="#ffffffeb" stroke="#dfe7f2"/><text x="${x + 16}" y="55" font-size="9" fill="#8597b1">${esc(t("Node {value0}", { value0: n }))}</text><text x="${center}" y="197" text-anchor="middle" font-size="9" fill="#8498b8">NVLink / ${summary.intra_node_topology === "fullmesh" ? "Full mesh" : "NVSwitch"}</text>`;
      const visible = Math.min(summary.gpus_per_node, 4);
      for (let g = 0; g < visible; g++) {
        const gx = visible > 2 ? center + (g % 2 ? 32 : -32) : center;
        const gy = visible > 2 ? 105 + Math.floor(g / 2) * 49 : 107 + g * 49;
        svg += gpu(gx, gy, `G${n * summary.gpus_per_node + g}`);
      }
      if (summary.gpus_per_node > 4)
        svg += `<text x="${center}" y="219" text-anchor="middle" font-size="8" fill="#8e9fb7">+ ${summary.gpus_per_node - 4} GPUs</text>`;
      svg += `<path d="M${center} 233v14" stroke="#df9c61" stroke-width="1.6"/>`;
    }
    svg += `<path d="M${start + boxWidth / 2} 247H${start + (shown - 1) * (boxWidth + gap) + boxWidth / 2}" stroke="#df9c61" stroke-width="1.6"/><text x="350" y="266" text-anchor="middle" font-size="8" fill="#b28b66">${esc(summary.fabric)} fabric · ${esc(summary.inter_node_topology)}${summary.num_nodes > shown ? t(" · showing {value0} of {value1} nodes", { value0: shown, value1: summary.num_nodes }) : ""}</text>`;
  } else if (summary.stencil) {
    const x1 = 210,
      x2 = 490;
    const rankLabel = (expression) => {
      const text = String(expression ?? "?").trim();
      if (/^\d+$/.test(text)) return `G${text}`;
      if (/^\d+\s+mod\s+N$/.test(text)) return `G${parseInt(text, 10) % count}`;
      if (/^N\s*-\s*1$/.test(text)) return `G${count - 1}`;
      return text.length > 9 ? text.slice(0, 8) + "…" : text;
    };
    const first = summary.transfers[0] || {};
    svg +=
      gpu(x1, 120, rankLabel(first.src)) + gpu(x2, 120, rankLabel(first.dst));
    svg += `<path d="M242 107H457" stroke="#7b9fee" stroke-width="1.7" marker-end="url(#network-arrow)"/><text x="350" y="88" text-anchor="middle" font-size="10" fill="#7291c5">${esc(summary.transfers[0]?.bytes || "dataSize")} bytes</text>`;
    if (
      summary.transfers.length > 1 &&
      rankLabel(summary.transfers[1].src) === rankLabel(first.dst) &&
      rankLabel(summary.transfers[1].dst) === rankLabel(first.src)
    )
      svg += `<path d="M457 144H242" stroke="#7b9fee" stroke-width="1.7" marker-end="url(#network-arrow)"/><text x="350" y="168" text-anchor="middle" font-size="10" fill="#7291c5">${esc(summary.transfers[1]?.bytes)} bytes · ${esc(t("After the preceding transfer is delivered"))}</text>`;
    svg += `<text x="350" y="221" text-anchor="middle" font-size="10" fill="#97a6bc">${count} ranks · ${esc(t("{value0} transfer definitions", { value0: summary.transfers.length }))}${summary.transfers.length > 2 ? t(" · first endpoint pair shown") : ""}</text>`;
  } else if (["ring", "switched", "fullmesh"].includes(summary.topology)) {
    const n = Math.min(count, 16),
      ring = summary.topology === "ring",
      cx = 350,
      cy = 133,
      rx = 195,
      ry = 94;
    const nodes = Array.from({ length: n }, (_, i) => ({
      x: cx + rx * Math.cos(-Math.PI / 2 + (2 * Math.PI * i) / n),
      y: cy + ry * Math.sin(-Math.PI / 2 + (2 * Math.PI * i) / n),
    }));
    nodes.forEach((p, i) => {
      const targets =
        summary.topology === "fullmesh"
          ? nodes.slice(i + 1)
          : [ring ? nodes[(i + 1) % n] : { x: cx, y: cy }];
      targets.forEach((next) => {
        svg += `<path d="M${p.x} ${p.y}L${next.x} ${next.y}" stroke="#b2c6ee" stroke-width="1.1" opacity=".75"/>`;
      });
    });
    if (summary.topology === "switched")
      svg += `<rect x="286" y="111" width="128" height="43" rx="7" fill="#f3f7ff" stroke="#c3d4f5"/><text x="350" y="137" text-anchor="middle" font-size="11" fill="#5475ad">NVSwitch</text>`;
    else if (ring)
      svg += `<text x="350" y="126" text-anchor="middle" font-size="18" font-weight="550" fill="#446085">${esc(summary.collective === "allreduce" ? "AllReduce" : summary.collective)}</text><text x="350" y="151" text-anchor="middle" font-size="10" fill="#98abc5">${count} ranks · ${esc(summary.algorithm)}</text>`;
    nodes.forEach((p, i) => {
      svg += gpu(p.x, p.y, `G${i}`);
    });
    if (count > n)
      svg += `<text x="350" y="263" text-anchor="middle" font-size="8" fill="#97a6bc">${esc(t("Schematic: showing {value0} of {value1} ranks", { value0: n, value1: count }))}</text>`;
  } else {
    svg += `<rect x="190" y="74" width="320" height="124" rx="10" fill="#f8faff" stroke="#dbe5f4"/><text x="350" y="117" text-anchor="middle" font-size="20" fill="#536f99">${esc(summary.topology)}</text><text x="350" y="145" text-anchor="middle" font-size="11" fill="#8498b8">${count} ranks · ${esc(summary.algorithm)}</text><text x="350" y="176" text-anchor="middle" font-size="9" fill="#96a6bb">${esc(t("No structural diagram is available for this topology. See the input snapshot for link settings."))}</text>`;
  }
  $("topology-canvas").innerHTML = svg + "</svg>";
  $("topology-canvas").setAttribute(
    "aria-label",
    t("Topology schematic: {value0} ranks, {value1} nodes, {value2}", {
      value0: summary.ranks,
      value1: summary.num_nodes,
      value2: summary.topology,
    }),
  );
  $("topology-badge").textContent =
    `${summary.num_nodes > 1 ? summary.inter_node_topology : summary.stencil ? t("Transfer stencil") : summary.topology} · ${summary.fabric}`;
  $("network-operation").textContent = summary.stencil
    ? t("Transfer stencil")
    : `${summary.collective} / ${summary.algorithm}`;
  $("network-scale").textContent =
    `${count} GPUs / ${summary.num_nodes} ${t(summary.num_nodes > 1 ? "nodes" : "node")}${summary.num_nodes > 1 ? ` · So ${summary.inter_node_bandwidth_gbps} Gbps / ${summary.inter_node_latency_ns} ns` : ""}`;
  document.querySelector(".effective-strip>div>span").textContent =
    summary.num_nodes > 1
      ? t("Intra-node bandwidth / link")
      : t("Effective bandwidth / link");
  $("so-legend").hidden = summary.num_nodes < 2;
}

function setTab(tab) {
  state.tab = tab;
  document.querySelectorAll("[data-tab]").forEach((button) => {
    const selected = button.dataset.tab === tab;
    button.setAttribute("aria-selected", String(selected));
    button.tabIndex = selected ? 0 : -1;
    const panel = $("tab-" + button.dataset.tab);
    panel.setAttribute("aria-labelledby", button.id);
  });
  for (const key of ["overview", "trace", "log", "snapshot"])
    $("tab-" + key).hidden = key !== tab;
  if (tab === "trace") loadTrace();
  if (tab === "log") loadLog();
}
function savedInputs(run) {
  return {
    ...run.request,
    name: run.name,
    config_text: run.config,
    profile_text: run.profile,
  };
}
async function selectRun(identifier, scroll = false, restoreInputs = false) {
  try {
    const run = await api(`/api/runs/${identifier}`);
    if (state.selected?.id !== identifier) {
      state.log = "";
      state.logCursor = 0;
      state.traceLoaded = null;
      $("run-log").textContent = t("Waiting for simulator output…");
    }
    state.selected = run;
    if (restoreInputs)
      loadConfig(run.request.config_id, savedInputs(run), false);
    history.replaceState(null, "", `#run=${identifier}`);
    renderSelected();
    renderHistory(true);
    await loadLog();
    if (state.tab === "trace") await loadTrace();
    if (scroll)
      document
        .querySelector(".results-panel")
        .scrollIntoView({ behavior: "smooth", block: "center" });
  } catch (error) {
    notify(error);
  }
}
function renderSelected() {
  const run = state.selected;
  if (!run) return;
  const active = !terminal.has(run.status),
    result = run.result || {},
    complete = run.status === "completed";
  $("result-empty").hidden = true;
  $("result-content").hidden = false;
  $("selected-run-description").textContent =
    `${displayRunName(run)} · ${run.summary.collective} / ${run.summary.algorithm} · ${run.summary.ranks} ranks · ${bytesLabel(run.summary.data_size)}${run.summary.stencil ? " dataSize" : " / rank"}`;
  const status = $("selected-run-status");
  status.hidden = false;
  status.className = `status-badge ${run.status}`;
  status.textContent = t(statusLabels[run.status] || run.status);
  $("run-progress").hidden = !active;
  const elapsed = run.started_at
    ? Math.max(0, (Date.now() - new Date(run.started_at).getTime()) / 1000)
    : 0;
  $("progress-text").textContent =
    run.status === "queued"
      ? t("Queued. Waiting for the preceding run to finish.")
      : t("{value0} · elapsed {value1} s", {
          value0: t(statusLabels[run.status]),
          value1: elapsed.toFixed(1),
        });
  $("run-error").hidden = !run.error;
  $("run-error").textContent = t(run.error);
  $("metric-duration").textContent = complete
    ? (result.simTimeNs / 1000).toLocaleString("en-US", {
        maximumFractionDigits: 3,
      })
    : "—";
  $("metric-exact").textContent = complete
    ? `${number(result.simTimeNs)} ns · status=complete`
    : active
      ? t("Waiting for the simulation to finish")
      : t("No completed-operation time is available");
  $("metric-packets").textContent = number(result.endpointTxPackets);
  $("metric-received").textContent = t("Endpoint RX {value0}", {
    value0: number(result.endpointRxPackets),
  });
  $("metric-retries").textContent = number(result.retransmittedPackets);
  $("metric-drops").textContent = `/ ${number(result.switchDroppedPackets)}`;
  const settings = run.summary.settings || {};
  const enabled = (key) => ["1", "true"].includes(String(settings[key]));
  const controlResults = [];
  if (Number(settings.switchPfcMode) > 0 || result.pfcPausesSent > 0) {
    controlResults.push(
      ["PFC PAUSE", result.pfcPausesSent],
      ["PFC RESUME", result.pfcResumesSent],
    );
  }
  if (enabled("dcqcn") || result.ecnMarkedPackets > 0) {
    controlResults.push(
      [t("ECN marks"), result.ecnMarkedPackets],
      [t("CNP feedback"), result.cnpPacketsReceived],
    );
  }
  if (enabled("rttcc") || result.rttccRttSamples > 0)
    controlResults.push([t("RTT samples"), result.rttccRttSamples]);
  if (enabled("llrEnabled") || result.permanentLosses > 0)
    controlResults.push([t("Permanent losses"), result.permanentLosses]);
  if (enabled("fecEnabled") && !run.summary.stencil)
    controlResults.push(
      [t("FEC correctable"), result.fecCorrectable],
      [t("FEC uncorrectable"), result.fecUncorrectable],
    );
  if (result.opticalInterSwitchLinks > 0)
    controlResults.push([
      t("Available optical links"),
      `${result.operationalOpticalInterSwitchLinks} / ${result.opticalInterSwitchLinks}`,
    ]);
  $("control-results").hidden = active || !controlResults.length || !run.result;
  $("control-results").innerHTML = controlResults
    .map(
      ([key, value]) =>
        `<div><span>${esc(key)}</span><strong>${esc(typeof value === "number" ? number(value) : (value ?? "—"))}</strong></div>`,
    )
    .join("");
  $("run-timing").textContent =
    run.wall_seconds != null
      ? t("Wall time {value0} s · RngRun={value1}", {
          value0: run.wall_seconds.toFixed(3),
          value1: run.request.seed,
        })
      : t("Wall time is recorded when the run finishes");
  $("run-identifier").textContent =
    `${displayRunName(run)} · ${run.id.slice(0, 8)}`;
  $("cancel-button").hidden = !active;
  $("cancel-button").disabled = run.status === "cancelling";
  $("download-run").hidden = active;
  $("download-run").href = `/api/runs/${run.id}/artifacts/run.zip`;
  $("download-log").hidden = false;
  $("download-log").href = `/api/runs/${run.id}/artifacts/stdout.log`;
  const entries = Object.entries(result);
  $("result-fields").innerHTML = entries
    .map(
      ([key, value]) => `<tr><td>${esc(key)}</td><td>${esc(value)}</td></tr>`,
    )
    .join("");
  $("result-field-count").textContent = t("{value0} fields", {
    value0: entries.length,
  });
  $("run-snapshot").textContent =
    `# ${run.name}\n# branch: ${run.source.branch}\n# commit: ${run.source.commit}\n# binary SHA256: ${run.binary?.sha256 || t("Waiting to start")}\n\n${run.config}\n# ---- profile.profile ----\n${run.profile}\n# ---- argv ----\n${JSON.stringify(run.command, null, 2)}`;
}
async function loadLog() {
  const run = state.selected;
  if (!run) return;
  try {
    const data = await api(`/api/runs/${run.id}/log?offset=${state.logCursor}`);
    if (state.selected?.id !== run.id) return;
    state.logCursor = data.cursor;
    state.log += data.text.replace(/\x1b\[[0-9;]*[a-zA-Z]/g, "");
    const truncated = state.log.length > 256000;
    if (truncated) state.log = state.log.slice(-256000);
    const pane = $("run-log"),
      atBottom = pane.scrollHeight - pane.scrollTop - pane.clientHeight < 50;
    pane.textContent =
      (truncated
        ? t(
            "[Showing the end of the log. Download the complete log for all output.]\n",
          )
        : "") + (state.log || t("Waiting for simulator output…"));
    if (atBottom) pane.scrollTop = pane.scrollHeight;
  } catch (error) {
    if (state.tab === "log") notify(error);
  }
}
async function loadTrace() {
  const run = state.selected;
  if (!run) return;
  const key = run.id + ":" + run.status + ":" + I18n.language;
  if (state.traceLoaded === key && terminal.has(run.status)) return;
  try {
    const data = await api(`/api/runs/${run.id}/trace`);
    if (state.selected?.id !== run.id) return;
    state.traceLoaded = key;
    if (!data.rows.length) {
      $("trace-content").innerHTML =
        `<div class="small-empty">${terminal.has(run.status) ? t("This scenario has no step trace. See the raw log for its results.") : t("Waiting for the injector to write step records…")}</div>`;
      return;
    }
    const ranks = [...new Set(data.rows.map((row) => row.gpu))]
      .sort((a, b) => a - b)
      .slice(0, 16);
    const width = 680,
      left = 58,
      right = 23,
      top = 25,
      height = Math.max(160, ranks.length * 26 + 65);
    const max = Math.max(1, ...data.rows.map((row) => row.sinceStartNs)) / 1000;
    const x = (t) => left + (width - left - right) * (t / max);
    let svg = `<svg viewBox="0 0 ${width} ${height}" xmlns="http://www.w3.org/2000/svg" aria-label="${esc(t("Step-completion timestamps for each rank"))}">`;
    for (let i = 0; i <= 4; i++) {
      const value = (max * i) / 4;
      svg += `<path d="M${x(value)} ${top - 8}V${height - 36}" stroke="#e8edf5" stroke-dasharray="3 4"/><text x="${x(value)}" y="${height - 16}" text-anchor="middle" font-size="9" fill="#8c9eb9">${+value.toFixed(2)}${i === 4 ? " µs" : ""}</text>`;
    }
    ranks.forEach((rank, index) => {
      const y = top + index * 26;
      svg += `<text x="0" y="${y + 3}" font-size="9" fill="#7f93b0">rank ${rank}</text><path d="M${left} ${y}H${width - right}" stroke="#f0f3f8"/>`;
      data.rows
        .filter((row) => row.gpu === rank)
        .slice(0, 256)
        .forEach((row) => {
          svg += `<circle cx="${x(row.sinceStartNs / 1000)}" cy="${y}" r="3" fill="#618bee" opacity=".85"><title>${esc(t("rank {value0}, step {value1}: {value2} ns", { value0: row.gpu, value1: row.step, value2: row.sinceStartNs }))}</title></circle>`;
        });
    });
    svg += "</svg>";
    if (
      data.truncated ||
      ranks.length < new Set(data.rows.map((r) => r.gpu)).size
    )
      svg += `<p class="field-hint">${esc(t("The chart shows at most 16 ranks. Download steps.csv for the complete trace."))}</p>`;
    $("trace-content").innerHTML = svg;
  } catch (error) {
    $("trace-content").textContent = messageText(error);
  }
}

function renderHistory(force = false) {
  const key =
    JSON.stringify(
      state.runs.map((run) => [run.id, run.status, run.result?.simTimeNs]),
    ) + state.selected?.id;
  if (!force && key === state.historyKey) return;
  state.historyKey = key;
  $("history-count").textContent = state.runs.length;
  if (!state.runs.length) return;
  $("history-rows").innerHTML = state.runs
    .map(
      (run) =>
        `<tr class="${run.id === state.selected?.id ? "current" : ""}"><td class="checkbox-cell"><input type="checkbox" data-compare="${run.id}" aria-label="${esc(t("Select {value0} for comparison", { value0: displayRunName(run) }))}" ${run.status !== "completed" ? "disabled" : ""} ${state.compared.has(run.id) ? "checked" : ""}></td><td><button class="run-link" data-run="${run.id}" type="button">${esc(displayRunName(run))}</button><small>${esc(run.id.slice(0, 8))} · ${esc(run.summary.algorithm)}</small></td><td class="numeric">${run.summary.ranks} ranks${run.summary.num_nodes > 1 ? `<small>${run.summary.num_nodes} × ${run.summary.gpus_per_node} GPUs</small>` : ""}</td><td class="numeric">${esc(bytesLabel(run.summary.data_size))}</td><td class="numeric duration">${run.status === "completed" ? durationLabel(run.result?.simTimeNs) : "—"}</td><td>${badge(run.status)}</td><td class="numeric">${new Date(run.created_at).toLocaleTimeString(I18n.language === "zh" ? "zh-CN" : "en-GB", { hour: "2-digit", minute: "2-digit", second: "2-digit" })}<small>${run.wall_seconds != null ? t("{value0} s wall", { value0: run.wall_seconds.toFixed(3) }) : t("Waiting for completion")}</small></td><td><button type="button" class="icon-button" data-clone="${run.id}" title="${esc(t("Load this experiment's inputs"))}" aria-label="${esc(t("Load inputs for {value0}", { value0: displayRunName(run) }))}"><svg><use href="#i-reset"/></svg></button></td></tr>`,
    )
    .join("");
}
function updateComparison() {
  $("compare-count").textContent = state.compared.size;
  $("compare-button").disabled = state.compared.size < 2;
}
function showComparison() {
  const runs = state.runs.filter(
    (run) => state.compared.has(run.id) && run.status === "completed",
  );
  if (runs.length < 2) return;
  const max = Math.max(1, ...runs.map((run) => run.result.simTimeNs));
  $("comparison-chart").innerHTML = runs
    .map(
      (run) =>
        `<div class="comparison-item"><div class="comparison-label"><span>${esc(displayRunName(run))}</span><strong>${durationLabel(run.result.simTimeNs)}</strong></div><div class="comparison-track"><div class="comparison-bar" style="width:${(run.result.simTimeNs / max) * 100}%"></div></div><small>${esc(run.summary.collective)} / ${esc(run.summary.algorithm)} · ${run.summary.ranks} ranks · ${esc(bytesLabel(run.summary.data_size))}${run.summary.stencil ? " dataSize" : " / rank"} · ${run.id.slice(0, 8)}</small></div>`,
    )
    .join("");
  if (!$("compare-dialog").open) $("compare-dialog").showModal();
}
async function poll() {
  if (polling || !state.catalog) return;
  polling = true;
  try {
    const [engine, list] = await Promise.all([
      api("/api/status"),
      api("/api/runs"),
    ]);
    renderEngine(engine);
    state.runs = list.runs;
    renderHistory();
    if (state.selected) {
      const run = await api(`/api/runs/${state.selected.id}`);
      if (run.id === state.selected?.id) {
        const changed = run.status !== state.selected.status;
        state.selected = run;
        renderSelected();
        await loadLog();
        if (state.tab === "trace" || (changed && terminal.has(run.status)))
          await loadTrace();
      }
    }
    $("connection-error").hidden = engine.ready;
  } catch (error) {
    state.connectionMessage = () =>
      t("Connection lost: {value0}", { value0: messageText(error) });
    $("connection-error").textContent = state.connectionMessage();
    $("connection-error").hidden = false;
    state.ready = false;
    if (state.engine)
      state.engine = { ...state.engine, ready: false, connected: false };
    $("run-button").disabled = true;
  } finally {
    polling = false;
  }
}
function renderEngine(engine) {
  state.engine = engine;
  state.ready = engine.ready;
  $("engine-state").className =
    `engine-state ${engine.ready ? "ready" : "unready"}`;
  $("engine-state").innerHTML =
    `<i></i>${engine.connected === false ? t("Connection lost") : engine.ready ? t("Simulator ready") : t("Build the simulator first")}`;
  $("branch-label").textContent = engine.branch;
  $("branch-label").title = `${engine.branch} · ${engine.commit}`;
  $("queue-label").textContent =
    engine.running || engine.queued
      ? t("Running {value0} · queued {value1}", {
          value0: engine.running,
          value1: engine.queued,
        })
      : t("One simulation at a time");
  $("run-button").disabled =
    !state.ready ||
    !state.preview ||
    state.submitting ||
    !$("form-error").hidden;
  if (!engine.ready && engine.connected !== false) {
    state.connectionMessage = () =>
      t(
        "Simulator binary not found. Run ./ns3 build gpu-cluster-sim from the repository root.",
      );
    $("connection-error").textContent = state.connectionMessage();
    $("connection-error").hidden = false;
  }
}

async function initialize() {
  await I18n.ready;
  try {
    const [catalog, engine, list] = await Promise.all([
      api("/api/catalog"),
      api("/api/status"),
      api("/api/runs"),
    ]);
    state.catalog = catalog;
    state.controls = new SimulationOptions(catalog.options, () => ({
      multi: state.scene === "multinode",
    }));
    I18n.apply();
    state.runs = list.runs;
    renderEngine(engine);
    setScene("collective");
    renderHistory();
    $("parameter-list").innerHTML = Object.entries(catalog.cli_fields)
      .map(
        ([key, field]) =>
          `<div class="parameter-row"><span>${esc(key)}</span><span>${esc(field.choices?.join(" / ") || (field.type === "bool" ? "0 / 1" : `${field.type} · ${field.min}–${field.max}`))}</span></div>`,
      )
      .join("");
    const match = location.hash.match(/^#run=([0-9a-f]{32})$/);
    if (match) await selectRun(match[1], false, true);
    setInterval(poll, 1200);
  } catch (error) {
    for (const id of [
      "collective-select",
      "algorithm-select",
      "physical-topology",
    ]) {
      const input = $(id);
      if (input.options.length <= 1) {
        input.disabled = true;
        input.innerHTML = `<option>${esc(t("Unable to load choices. Reload the page."))}</option>`;
      }
    }
    state.connectionMessage = () =>
      t("Cannot load the workspace: {value0}", { value0: messageText(error) });
    $("connection-error").textContent = state.connectionMessage();
    $("connection-error").hidden = false;
  }
}

document.querySelectorAll("[data-scene]").forEach((button) =>
  button.addEventListener("click", () => {
    if (state.catalog) setScene(button.dataset.scene);
  }),
);
$("config-select").addEventListener("change", (event) =>
  loadConfig(event.target.value),
);
$("reset-button").addEventListener("click", () => {
  if (state.config) {
    loadConfig(state.config.id);
    notify(t("Template defaults restored"));
  }
});
document.querySelectorAll("[data-gpus]").forEach((button) =>
  button.addEventListener("click", () => {
    $("num-gpus").value = button.dataset.gpus;
    schedulePreview();
  }),
);
$("simulation-form").addEventListener("input", schedulePreview);
$("simulation-form").addEventListener("change", schedulePreview);
$("data-unit").addEventListener("change", updateLocalLabels);
$("apply-source").addEventListener("click", async () => {
  if (await refreshPreview()) notify(t("Inputs validated. Preview updated."));
});
$("simulation-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (
    state.submitting ||
    !state.ready ||
    !$("simulation-form").reportValidity()
  )
    return;
  state.submitting = true;
  $("run-button").disabled = true;
  formError();
  try {
    const run = await api("/api/runs", {
      method: "POST",
      body: JSON.stringify(requestBody()),
    });
    setTab("overview");
    await selectRun(run.id);
    await poll();
    notify(t("Simulation submitted"));
  } catch (error) {
    formError(
      error instanceof SyntaxError
        ? t("Invalid JSON in Extra CLI flags.")
        : error,
    );
  } finally {
    state.submitting = false;
    $("run-button").disabled = !state.ready || !$("form-error").hidden;
  }
});
$("cancel-button").addEventListener("click", async () => {
  if (!state.selected) return;
  try {
    state.selected = await api(`/api/runs/${state.selected.id}/cancel`, {
      method: "POST",
    });
    renderSelected();
    await poll();
  } catch (error) {
    notify(error);
  }
});
document.querySelectorAll("[data-tab]").forEach((button) => {
  button.addEventListener("click", () => setTab(button.dataset.tab));
  button.addEventListener("keydown", (event) => {
    const tabs = [...document.querySelectorAll("[data-tab]")];
    let index = tabs.indexOf(button);
    if (event.key === "ArrowRight") index = (index + 1) % tabs.length;
    else if (event.key === "ArrowLeft")
      index = (index + tabs.length - 1) % tabs.length;
    else if (event.key === "Home") index = 0;
    else if (event.key === "End") index = tabs.length - 1;
    else return;
    event.preventDefault();
    setTab(tabs[index].dataset.tab);
    tabs[index].focus();
  });
});
$("history-rows").addEventListener("click", async (event) => {
  const open = event.target.closest("[data-run]");
  if (open) await selectRun(open.dataset.run, true, true);
  const clone = event.target.closest("[data-clone]");
  if (clone) {
    try {
      const run = await api(`/api/runs/${clone.dataset.clone}`);
      loadConfig(run.request.config_id, savedInputs(run));
      $("config-panel").scrollIntoView({ behavior: "smooth", block: "start" });
      notify(t("Inputs loaded. Edit them and run again."));
    } catch (error) {
      notify(error);
    }
  }
});
$("history-rows").addEventListener("change", (event) => {
  const box = event.target.closest("[data-compare]");
  if (!box) return;
  if (box.checked) {
    if (state.compared.size >= 6) {
      box.checked = false;
      notify(t("Compare at most 6 runs at a time."));
      return;
    }
    state.compared.add(box.dataset.compare);
  } else state.compared.delete(box.dataset.compare);
  updateComparison();
});
$("compare-button").addEventListener("click", showComparison);
document
  .querySelectorAll("[data-help]")
  .forEach((button) =>
    button.addEventListener("click", () => $("help-dialog").showModal()),
  );
$("parameter-help").addEventListener("click", () =>
  $("parameters-dialog").showModal(),
);
document
  .querySelectorAll("[data-close]")
  .forEach((button) =>
    button.addEventListener("click", () => button.closest("dialog").close()),
  );
document.querySelectorAll("dialog").forEach((dialog) =>
  dialog.addEventListener("click", (event) => {
    if (event.target === dialog) {
      const r = dialog.getBoundingClientRect();
      if (
        event.clientX < r.left ||
        event.clientX > r.right ||
        event.clientY < r.top ||
        event.clientY > r.bottom
      )
        dialog.close();
    }
  }),
);
$("copy-config").addEventListener("click", async () => {
  if (!state.selected) {
    notify(t("Select a run first."));
    return;
  }
  try {
    await navigator.clipboard.writeText(state.selected.config);
    notify(t("Run configuration copied"));
  } catch {
    notify(
      t(
        "Clipboard access was denied. Download config.cfg from the run archive.",
      ),
    );
  }
});
for (const [input, editor] of [
  ["import-config", "config-editor"],
  ["import-profile", "profile-editor"],
])
  $(input).addEventListener("change", async (event) => {
    const file = event.target.files[0];
    if (!file) return;
    if (file.size > 65536) {
      notify(t("Files must not exceed 64 KiB."));
      return;
    }
    $(editor).value = (await file.text()).replace(/^\uFEFF/, "");
    schedulePreview();
    notify(t("Loaded {value0}", { value0: file.name }));
    event.target.value = "";
  });
document.addEventListener("keydown", (event) => {
  if (
    (event.ctrlKey || event.metaKey) &&
    event.key === "Enter" &&
    !document.querySelector("dialog[open]")
  ) {
    event.preventDefault();
    if (!$("run-button").disabled) $("simulation-form").requestSubmit();
  }
});
window.addEventListener("hashchange", () => {
  const match = location.hash.match(/^#run=([0-9a-f]{32})$/);
  if (match && state.catalog) selectRun(match[1], false, true);
});
document.addEventListener("languagechange", () => {
  formError(state.formMessage);
  $("toast").textContent = messageText(state.toastMessage);
  if (state.connectionMessage)
    $("connection-error").textContent = state.connectionMessage();
  if (!state.catalog) return;
  const configSelect = $("config-select");
  for (const option of configSelect.options) {
    const config = state.catalog.configs.find(
      (item) => item.id === option.value,
    );
    if (config) option.textContent = t(config.title);
  }
  $("config-count").textContent = t("{value0} templates", {
    value0: configSelect.options.length,
  });
  $("config-description").textContent = t(state.config.description);
  state.controls.relocalize();
  updateLocalLabels();
  if (state.preview) {
    const preview = state.preview;
    document.querySelector('label[for="data-size"]').textContent = t(
      preview.summary.stencil ? "dataSize input" : "Application bytes per rank",
    );
    $("preview-notices").textContent = preview.notices
      .map((message) => t(message))
      .join(" ");
    $("stencil-hint").textContent = t(
      preview.notices[0] ||
        "The transfer.*.bytes expressions determine the actual transfer sizes.",
    );
    renderTopology(preview.summary);
  }
  if (state.engine) renderEngine(state.engine);
  renderSelected();
  renderHistory(true);
  if (state.selected) {
    if (state.tab === "trace") loadTrace();
    $("run-log").textContent = state.log || t("Waiting for simulator output…");
  }
  if ($("compare-dialog").open) showComparison();
});
initialize();
