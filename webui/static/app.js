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
  queued: "排队中",
  running: "运行中",
  cancelling: "正在取消",
  completed: "已完成",
  incomplete: "未完成",
  failed: "运行失败",
  cancelled: "已取消",
  timed_out: "已超时",
  interrupted: "已中断",
};
const state = {
  catalog: null,
  scene: "collective",
  config: null,
  baseStack: {},
  ready: false,
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
};
let previewTimer,
  toastTimer,
  polling = false;

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
    throw new Error("服务器响应无法读取，请检查服务是否仍在运行。");
  }
  if (!response.ok) {
    const message = Array.isArray(data.detail)
      ? data.detail
          .map((x) => `${x.loc.slice(1).join(".")}: ${x.msg}`)
          .join("；")
      : data.detail;
    throw new Error(message || `请求失败 (${response.status})`);
  }
  return data;
}
function notify(message) {
  $("toast").textContent = message;
  $("toast").hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    $("toast").hidden = true;
  }, 3500);
}
function formError(message = "") {
  $("form-error").textContent = message;
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
  return `<span class="status-badge ${esc(status)}">${esc(statusLabels[status] || status)}</span>`;
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
        `<option value="${esc(config.id)}">${esc(config.title)}</option>`,
    )
    .join("");
  $("config-count").textContent = `${configs.length} 个模板`;
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
  $("config-description").textContent = config.description;
  $("config-editor").value = request?.config_text ?? config.text;
  $("profile-editor").value = request?.profile_text ?? config.profile_text;
  const defaults = { ...config.defaults, ...request };
  state.baseStack = { ...(defaults.stack_overrides || {}) };
  $("num-gpus").value = defaults.num_gpus ?? 8;
  $("num-nodes").value = defaults.num_nodes > 1 ? defaults.num_nodes : 2;
  $("gpus-per-node").value = defaults.gpus_per_node ?? 4;
  $("seed").value = defaults.seed ?? 1;
  $("timeout").value = defaults.timeout ?? 120;
  $("run-name").value = request
    ? `${request.name || config.title}${copyName ? " · 副本" : ""}`.slice(0, 80)
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
    throw new Error("附加 CLI 参数需为 JSON 对象。");
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
  return {
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
    cli_overrides: cli,
  };
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
    document.querySelector('label[for="data-size"]').textContent = result
      .summary.stencil
      ? "dataSize 输入参数"
      : "每 rank 应用数据量";
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
    $("preview-notices").textContent = result.notices.join(" ");
    $("preview-notices").hidden = !result.notices.length;
    $("stencil-hint").hidden = !result.summary.stencil;
    $("stencil-hint").textContent =
      result.notices[0] || "实际传输量由配置中的 transfer.*.bytes 表达式决定。";
    $("run-button").disabled = !state.ready || state.submitting;
    return true;
  } catch (error) {
    if (version === state.previewVersion) {
      $("run-button").disabled = true;
      formError(
        error instanceof SyntaxError
          ? "附加 CLI 参数的 JSON 格式无效。"
          : error.message,
      );
    }
    return false;
  }
}
function schedulePreview() {
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
      svg += `<rect x="${x}" y="32" width="${boxWidth}" height="201" rx="10" fill="#ffffffeb" stroke="#dfe7f2"/><text x="${x + 16}" y="55" font-size="9" fill="#8597b1">NODE ${n}</text><text x="${center}" y="197" text-anchor="middle" font-size="9" fill="#8498b8">NVLink / NVSwitch</text>`;
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
    svg += `<path d="M${start + boxWidth / 2} 247H${start + (shown - 1) * (boxWidth + gap) + boxWidth / 2}" stroke="#df9c61" stroke-width="1.6"/><text x="350" y="266" text-anchor="middle" font-size="8" fill="#b28b66">${esc(summary.fabric)} fabric · ${esc(summary.inter_node_topology)}${summary.num_nodes > shown ? ` · 展示前 ${shown} 个节点，共 ${summary.num_nodes} 个` : ""}</text>`;
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
      svg += `<path d="M457 144H242" stroke="#7b9fee" stroke-width="1.7" marker-end="url(#network-arrow)"/><text x="350" y="168" text-anchor="middle" font-size="10" fill="#7291c5">${esc(summary.transfers[1]?.bytes)} bytes · 前一传输交付后</text>`;
    svg += `<text x="350" y="221" text-anchor="middle" font-size="10" fill="#97a6bc">${count} ranks · ${esc(summary.transfers.length)} 个传输定义${summary.transfers.length > 2 ? " · 仅预览首组端点" : ""}</text>`;
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
      svg += `<text x="350" y="263" text-anchor="middle" font-size="8" fill="#97a6bc">简化示意：展示前 ${n} 个 rank，共 ${count} 个</text>`;
  } else {
    svg += `<rect x="190" y="74" width="320" height="124" rx="10" fill="#f8faff" stroke="#dbe5f4"/><text x="350" y="117" text-anchor="middle" font-size="20" fill="#536f99">${esc(summary.topology)}</text><text x="350" y="145" text-anchor="middle" font-size="11" fill="#8498b8">${count} ranks · ${esc(summary.algorithm)}</text><text x="350" y="176" text-anchor="middle" font-size="9" fill="#96a6bb">此拓扑暂无结构图，连接参数见配置快照。</text>`;
  }
  $("topology-canvas").innerHTML = svg + "</svg>";
  $("topology-canvas").setAttribute(
    "aria-label",
    `${summary.ranks} ranks，${summary.num_nodes} 个节点，${summary.topology} 拓扑示意`,
  );
  $("topology-badge").textContent =
    `${summary.num_nodes > 1 ? summary.inter_node_topology : summary.stencil ? "Transfer stencil" : summary.topology} · ${summary.fabric}`;
  $("network-operation").textContent = summary.stencil
    ? "Transfer stencil"
    : `${summary.collective} / ${summary.algorithm}`;
  $("network-scale").textContent =
    `${count} GPU / ${summary.num_nodes} ${summary.num_nodes > 1 ? "nodes" : "node"}${summary.num_nodes > 1 ? ` · So ${summary.inter_node_bandwidth_gbps} Gbps / ${summary.inter_node_latency_ns} ns` : ""}`;
  document.querySelector(".effective-strip>div>span").textContent =
    summary.num_nodes > 1 ? "节点内带宽 / 链路" : "有效带宽 / 链路";
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
      $("run-log").textContent = "等待程序输出…";
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
    notify(error.message);
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
    `${run.name} · ${run.summary.collective} / ${run.summary.algorithm} · ${run.summary.ranks} ranks · ${bytesLabel(run.summary.data_size)}${run.summary.stencil ? " dataSize" : " / rank"}`;
  const status = $("selected-run-status");
  status.hidden = false;
  status.className = `status-badge ${run.status}`;
  status.textContent = statusLabels[run.status] || run.status;
  $("run-progress").hidden = !active;
  const elapsed = run.started_at
    ? Math.max(0, (Date.now() - new Date(run.started_at).getTime()) / 1000)
    : 0;
  $("progress-text").textContent =
    run.status === "queued"
      ? "已进入队列，等待前一任务结束。"
      : `${statusLabels[run.status]} · 已运行 ${elapsed.toFixed(1)} s`;
  $("run-error").hidden = !run.error;
  $("run-error").textContent = run.error || "";
  $("metric-duration").textContent = complete
    ? (result.simTimeNs / 1000).toLocaleString("en-US", {
        maximumFractionDigits: 3,
      })
    : "—";
  $("metric-exact").textContent = complete
    ? `${number(result.simTimeNs)} ns · status=complete`
    : active
      ? "等待仿真完成"
      : "未取得已完成操作的时长";
  $("metric-packets").textContent = number(result.endpointTxPackets);
  $("metric-received").textContent =
    `端点接收 ${number(result.endpointRxPackets)}`;
  $("metric-retries").textContent = number(result.retransmittedPackets);
  $("metric-drops").textContent = `/ ${number(result.switchDroppedPackets)}`;
  $("run-timing").textContent =
    run.wall_seconds != null
      ? `程序运行 ${run.wall_seconds.toFixed(3)} s · RngRun=${run.request.seed}`
      : "完成时记录程序运行时间";
  $("run-identifier").textContent = `${run.name} · ${run.id.slice(0, 8)}`;
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
  $("result-field-count").textContent = `${entries.length} fields`;
  $("run-snapshot").textContent =
    `# ${run.name}\n# branch: ${run.source.branch}\n# commit: ${run.source.commit}\n# binary SHA256: ${run.binary?.sha256 || "等待启动"}\n\n${run.config}\n# ---- profile.profile ----\n${run.profile}\n# ---- argv ----\n${JSON.stringify(run.command, null, 2)}`;
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
      (truncated ? "[页面仅显示日志末尾，可下载完整日志]\n" : "") +
      (state.log || "等待程序输出…");
    if (atBottom) pane.scrollTop = pane.scrollHeight;
  } catch (error) {
    if (state.tab === "log") notify(error.message);
  }
}
async function loadTrace() {
  const run = state.selected;
  if (!run) return;
  const key = run.id + ":" + run.status;
  if (state.traceLoaded === key && terminal.has(run.status)) return;
  try {
    const data = await api(`/api/runs/${run.id}/trace`);
    if (state.selected?.id !== run.id) return;
    state.traceLoaded = key;
    if (!data.rows.length) {
      $("trace-content").innerHTML =
        `<div class="small-empty">${terminal.has(run.status) ? "此场景未记录步骤数据。可在原始日志中查看运行结果。" : "等待 injector 写入步骤数据…"}</div>`;
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
    let svg = `<svg viewBox="0 0 ${width} ${height}" xmlns="http://www.w3.org/2000/svg" aria-label="每个 rank 的步骤完成时刻">`;
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
          svg += `<circle cx="${x(row.sinceStartNs / 1000)}" cy="${y}" r="3" fill="#618bee" opacity=".85"><title>rank ${row.gpu}, step ${row.step}: ${row.sinceStartNs} ns</title></circle>`;
        });
    });
    svg += "</svg>";
    if (
      data.truncated ||
      ranks.length < new Set(data.rows.map((r) => r.gpu)).size
    )
      svg +=
        '<p class="field-hint">为便于阅读，最多显示 16 个 rank；完整数据见下载包中的 steps.csv。</p>';
    $("trace-content").innerHTML = svg;
  } catch (error) {
    $("trace-content").textContent = error.message;
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
        `<tr class="${run.id === state.selected?.id ? "current" : ""}"><td class="checkbox-cell"><input type="checkbox" data-compare="${run.id}" aria-label="选择 ${esc(run.name)} 进行比较" ${run.status !== "completed" ? "disabled" : ""} ${state.compared.has(run.id) ? "checked" : ""}></td><td><button class="run-link" data-run="${run.id}" type="button">${esc(run.name)}</button><small>${esc(run.id.slice(0, 8))} · ${esc(run.summary.algorithm)}</small></td><td class="numeric">${run.summary.ranks} ranks${run.summary.num_nodes > 1 ? `<small>${run.summary.num_nodes} × ${run.summary.gpus_per_node} GPUs</small>` : ""}</td><td class="numeric">${esc(bytesLabel(run.summary.data_size))}</td><td class="numeric duration">${run.status === "completed" ? durationLabel(run.result?.simTimeNs) : "—"}</td><td>${badge(run.status)}</td><td class="numeric">${new Date(run.created_at).toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit", second: "2-digit" })}<small>${run.wall_seconds != null ? run.wall_seconds.toFixed(3) + " s wall" : "等待完成"}</small></td><td><button type="button" class="icon-button" data-clone="${run.id}" title="载入此实验配置" aria-label="载入 ${esc(run.name)} 的配置"><svg><use href="#i-reset"/></svg></button></td></tr>`,
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
        `<div class="comparison-item"><div class="comparison-label"><span>${esc(run.name)}</span><strong>${durationLabel(run.result.simTimeNs)}</strong></div><div class="comparison-track"><div class="comparison-bar" style="width:${(run.result.simTimeNs / max) * 100}%"></div></div><small>${esc(run.summary.collective)} / ${esc(run.summary.algorithm)} · ${run.summary.ranks} ranks · ${esc(bytesLabel(run.summary.data_size))}${run.summary.stencil ? " dataSize" : " / rank"} · ${run.id.slice(0, 8)}</small></div>`,
    )
    .join("");
  $("compare-dialog").showModal();
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
    $("connection-error").textContent = `连接中断：${error.message}`;
    $("connection-error").hidden = false;
    state.ready = false;
    $("run-button").disabled = true;
  } finally {
    polling = false;
  }
}
function renderEngine(engine) {
  state.ready = engine.ready;
  $("engine-state").className =
    `engine-state ${engine.ready ? "ready" : "unready"}`;
  $("engine-state").innerHTML =
    `<i></i>${engine.ready ? "仿真引擎就绪" : "需要构建仿真程序"}`;
  $("branch-label").textContent = engine.branch;
  $("branch-label").title = `${engine.branch} · ${engine.commit}`;
  $("queue-label").textContent =
    engine.running || engine.queued
      ? `运行中 ${engine.running} · 排队 ${engine.queued}`
      : "单任务执行队列";
  $("run-button").disabled =
    !state.ready ||
    !state.preview ||
    state.submitting ||
    !$("form-error").hidden;
  if (!engine.ready) {
    $("connection-error").textContent =
      "尚未找到仿真程序。请先在仓库根目录运行：./ns3 build gpu-cluster-sim";
    $("connection-error").hidden = false;
  }
}

async function initialize() {
  try {
    const [catalog, engine, list] = await Promise.all([
      api("/api/catalog"),
      api("/api/status"),
      api("/api/runs"),
    ]);
    state.catalog = catalog;
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
    $("connection-error").textContent = `无法加载工作区：${error.message}`;
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
    notify("已恢复当前场景默认值");
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
  if (await refreshPreview()) notify("配置校验通过，预览已更新");
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
    notify("已提交仿真任务");
  } catch (error) {
    formError(
      error instanceof SyntaxError
        ? "附加 CLI 参数的 JSON 格式无效。"
        : error.message,
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
    notify(error.message);
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
      notify("已载入配置，可修改后重新运行");
    } catch (error) {
      notify(error.message);
    }
  }
});
$("history-rows").addEventListener("change", (event) => {
  const box = event.target.closest("[data-compare]");
  if (!box) return;
  if (box.checked) {
    if (state.compared.size >= 6) {
      box.checked = false;
      notify("一次最多对比 6 个实验");
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
    notify("请先选择一次运行");
    return;
  }
  try {
    await navigator.clipboard.writeText(state.selected.config);
    notify("已复制运行配置");
  } catch {
    notify("浏览器未允许复制，请从完整运行记录中下载 config.cfg");
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
      notify("文件不能超过 64 KiB");
      return;
    }
    $(editor).value = (await file.text()).replace(/^\uFEFF/, "");
    schedulePreview();
    notify(`已载入 ${file.name}`);
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
initialize();
