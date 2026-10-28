"use strict";

// The API supplies the same native choices/ranges used to validate each run.
class SimulationOptions {
  constructor(schema, context) {
    this.schema = schema;
    this.context = context;
    this.preview = null;
    this.fields = [
      ...schema.topology_fields,
      ...schema.groups.flatMap((group) => group.fields),
    ];
    const algorithmKeys = new Set([
      "intraNodeAlgorithm",
      "rootRank",
      "ringChannels",
    ]);
    this.algorithmFields = schema.topology_fields.filter((item) =>
      algorithmKeys.has(item.key),
    );
    this.topologyFields = schema.topology_fields.filter(
      (item) =>
        !algorithmKeys.has(item.key) && item.key !== "intraNodeTopology",
    );
    this.escape = (value) =>
      String(value ?? "").replace(
        /[&<>"']/g,
        (c) =>
          ({
            "&": "&amp;",
            "<": "&lt;",
            ">": "&gt;",
            '"': "&quot;",
            "'": "&#39;",
          })[c],
      );
    document.getElementById("topology-option-fields").innerHTML =
      this.topologyFields.map((item) => this.renderField(item)).join("");
    document.getElementById("algorithm-option-fields").innerHTML =
      this.algorithmFields.map((item) => this.renderField(item)).join("");
    document.getElementById("intra-topology-control").innerHTML =
      this.renderField(
        schema.topology_fields.find((item) => item.key === "intraNodeTopology"),
      );
    document.getElementById("native-options").innerHTML = schema.groups
      .map(
        (group) => `
      <details class="advanced option-group" id="options-${group.id}">
        <summary><svg><use href="#i-sliders"/></svg>${this.escape(group.title)}<span>${this.escape(group.subtitle)}</span></summary>
        <div class="advanced-content"><p class="field-hint">${this.escape(group.hint)}</p>
          ${group.id === "errors" ? '<div class="option-actions"><button type="button" class="secondary-button" id="clear-errors">设为无错误</button><button type="button" class="secondary-button" id="loss-sack-preset">0.1% 丢包 + SACK</button></div>' : ""}
          <div class="option-grid">${group.fields.map((item) => this.renderField(item)).join("")}</div>
        </div>
      </details>`,
      )
      .join("");
    document
      .getElementById("collective-select")
      .addEventListener("change", () => this.collectiveChanged());
    document
      .getElementById("physical-topology")
      .addEventListener("change", () => this.topologyChanged());
    document
      .getElementById("simulation-form")
      .addEventListener("focusout", (event) => {
        const input = event.target;
        if (input.tagName === "SELECT" && input.dataset.pendingDefaultLabel) {
          input.options[0].textContent = input.dataset.pendingDefaultLabel;
          delete input.dataset.pendingDefaultLabel;
        }
      });
    document.getElementById("clear-errors").addEventListener("click", () => {
      this.clearErrors();
      this.changed();
    });
    document
      .getElementById("loss-sack-preset")
      .addEventListener("click", () => {
        this.clearErrors();
        const values = {
          packetLossRate: "0.001",
          llrEnabled: "1",
          llrMode: "sack",
          llrTimeoutRetransmit: "1",
          llrRetryLimit: "50",
          llrBufferSize: "10000",
          vcCredits: "100000000",
          bulkChunkSize: "4096",
          llrRetryTimeout: "200000",
        };
        if (this.context().multi) values.soTxWindow = "64";
        for (const [key, value] of Object.entries(values))
          document.getElementById(`opt-${key}`).value = value;
        document.getElementById("options-reliability").open = true;
        this.changed();
      });
  }

  renderField(item) {
    const e = this.escape,
      id = `opt-${item.key}`;
    const input = item.choices
      ? `<select id="${id}" data-native-key="${item.key}"><option value="">沿用配置</option>${item.choices.map((choice) => `<option value="${e(choice.value)}">${e(choice.label)}</option>`).join("")}</select>`
      : `<input id="${id}" data-native-key="${item.key}" type="number" min="${item.min}" max="${item.max}" step="${item.type === "float" ? "any" : "1"}" placeholder="沿用配置" aria-describedby="${id}-effective ${id}-hint"/>`;
    return `<div class="field" id="${id}-field"><label for="${id}">${e(item.label)}</label>${input}<small class="option-effective" id="${id}-effective"></small>${item.hint ? `<p class="field-hint" id="${id}-hint">${e(item.hint)}</p>` : `<span id="${id}-hint"></span>`}</div>`;
  }

  source() {
    const parse = (text, sections) => {
      const result = { stack: {}, op: {}, "": {} };
      let section = "";
      for (let line of text.split(/\r?\n/)) {
        line = line.split("#")[0].trim();
        if (sections && /^\[(stack|op)\]$/.test(line))
          section = line.slice(1, -1);
        else if (line.includes("=")) {
          const at = line.indexOf("=");
          result[section][line.slice(0, at).trim()] = line.slice(at + 1).trim();
        }
      }
      return result;
    };
    const config = parse(document.getElementById("config-editor").value, true);
    return {
      op: config.op,
      stack: {
        ...parse(document.getElementById("profile-editor").value, false)[""],
        ...config.stack,
      },
    };
  }

  reset(request = {}) {
    this.preview = null;
    const stencil = !("collective" in this.source().op);
    for (const item of this.fields) {
      const target =
        item.target === "credits" ? (stencil ? "stack" : "cli") : item.target;
      let value = request[`${target}_overrides`]?.[item.key] ?? "";
      if (typeof value === "boolean") value = value ? "1" : "0";
      if (item.choices && ["true", "false"].includes(value))
        value = value === "true" ? "1" : "0";
      document.getElementById(`opt-${item.key}`).value = String(value);
    }
    this.setChoices(
      "collective-select",
      this.schema.collectives,
      request.op_overrides?.collective ?? "",
    );
    this.setChoices(
      "physical-topology",
      this.schema.topologies,
      request.op_overrides?.topology ?? "",
    );
    this.updateAlgorithms(request.op_overrides?.algorithm ?? "");
    this.sync();
  }

  setChoices(id, choices, value, inherited = "沿用配置") {
    const e = this.escape,
      input = document.getElementById(id);
    let markup =
      `<option value="">${e(inherited)}</option>` +
      choices
        .map(
          (item) =>
            `<option value="${e(item.value)}">${e(item.label)}</option>`,
        )
        .join("");
    if (value && !choices.some((item) => item.value === value)) {
      markup += `<option value="${e(value)}">${e(value)} · 请检查兼容性</option>`;
    }
    const signature = JSON.stringify([
      choices,
      value && !choices.some((item) => item.value === value) ? value : null,
    ]);
    if (input.dataset.choicesSignature !== signature) {
      input.innerHTML = markup;
      input.dataset.choicesSignature = signature;
    }
    if (input.value !== value) input.value = value;
  }

  setDefaultLabel(input, label) {
    if (input.options[0].textContent === label) {
      delete input.dataset.pendingDefaultLabel;
      return;
    }
    // Updating an open native select can dismiss its OS menu in Chrome.
    // Leave focused selects alone and apply the label after focus leaves.
    if (document.activeElement === input) {
      input.dataset.pendingDefaultLabel = label;
      return;
    }
    input.options[0].textContent = label;
  }

  operation() {
    const op = this.source().op;
    return {
      stencil: !("collective" in op),
      collective:
        document.getElementById("collective-select").value || op.collective,
      algorithm:
        document.getElementById("algorithm-select").value ||
        op.algorithm ||
        "ring",
      topology:
        document.getElementById("physical-topology").value ||
        op.topology ||
        "ring",
    };
  }

  updateAlgorithms(value = document.getElementById("algorithm-select").value) {
    const collective =
      document.getElementById("collective-select").value ||
      this.source().op.collective;
    const definition = this.schema.collectives.find(
      (item) => item.value === collective,
    );
    this.algorithmCollective = collective;
    this.setChoices(
      "algorithm-select",
      (definition?.algorithms || []).map((value) => ({
        value,
        label:
          collective === "alltoall" && ["ring", "fullmesh"].includes(value)
            ? `${value} · AlltoAllInjector`
            : this.schema.algorithms[value],
      })),
      value,
    );
  }

  collectiveChanged() {
    const previous = this.operation().algorithm;
    const collective =
      document.getElementById("collective-select").value ||
      this.source().op.collective;
    const choices =
      this.schema.collectives.find((item) => item.value === collective)
        ?.algorithms || [];
    this.updateAlgorithms(choices.includes(previous) ? previous : "ring");
  }

  topologyChanged() {
    const topology = document.getElementById("physical-topology").value;
    if (!topology) return;
    const n = Number(document.getElementById("num-gpus").value);
    const divisor = (count, bound) => {
      for (let value = Math.min(count, Math.floor(bound)); value >= 1; value--)
        if (count % value === 0) return value;
      return 1;
    };
    const leaves = divisor(n, 4),
      rows = divisor(n, Math.sqrt(n));
    const x = divisor(n, 2),
      y = divisor(n / x, 2);
    const groups = divisor(n, 2),
      routers = divisor(n / groups, 2);
    const values =
      {
        fattree: { fattreeRadix: 4 },
        railfattree: { railCount: divisor(n, 8), linksPerGpu: 1 },
        leafspine: { leafSwitches: leaves, spineSwitches: 2 },
        nvl72: { nvl72GpusPerGroup: divisor(n, Math.max(1, n / 6)) },
        torus: { torusDimX: x, torusDimY: y, torusDimZ: n / x / y },
        mesh: { meshRows: rows, meshCols: n / rows },
        hypercube: { hypercubeDims: Math.ceil(Math.log2(n)) },
        "2dfullmesh": { rackRows: rows, rackCols: n / rows },
        "2dfullmeshclos": {
          rackRows: divisor(n / groups, 2),
          rackCols: n / groups / divisor(n / groups, 2),
          numRacks: groups,
        },
        "3levelhierarchical": {
          leafSwitches: leaves,
          hier3MidSwitches: divisor(leaves, 2),
          spineSwitches: 2,
        },
        dragonflyplus: { dflyGroups: groups, dflyRoutersPerGroup: routers },
        multiplane: {
          multiPlaneCount: Math.min(
            4,
            Number(
              document.getElementById("opt-linksPerGpu").value ||
                this.source().stack.linksPerGpu ||
                1,
            ),
          ),
        },
      }[topology] || {};
    for (const [key, value] of Object.entries(values))
      document.getElementById(`opt-${key}`).value = String(value);
    if (Object.keys(values).length)
      document.getElementById("topology-options").open = true;
    this.sync();
  }

  extraCli(values) {
    const managed = new Set(
      this.fields
        .filter(
          (item) =>
            this.applies(item) &&
            (item.target === "cli" ||
              (item.target === "credits" && !this.operation().stencil)),
        )
        .map((item) => item.key),
    );
    return Object.fromEntries(
      Object.entries(values || {}).filter(([key]) => !managed.has(key)),
    );
  }

  applies(item, operation = this.operation()) {
    const { multi } = this.context();
    if (
      (item.scope === "single" && multi) ||
      (item.scope === "multi" && !multi) ||
      (item.scope === "stencil" && !operation.stencil) ||
      (item.scope === "collective" && operation.stencil)
    )
      return false;
    return true;
  }

  sync() {
    const { multi } = this.context(),
      operation = this.operation();
    if (this.algorithmCollective !== operation.collective)
      this.updateAlgorithms();
    document.getElementById("collective-field").hidden = operation.stencil;
    document.getElementById("algorithm-field").hidden = operation.stencil;
    document.getElementById("collective-select").disabled = operation.stencil;
    document.getElementById("algorithm-select").disabled = operation.stencil;
    document.getElementById("physical-topology-field").hidden = multi;
    document.getElementById("physical-topology").disabled = multi;
    document.getElementById("multi-topology-controls").hidden = !multi;
    document.getElementById("stencil-operation-hint").hidden =
      !operation.stencil;
    const topology = this.schema.topologies.find(
      (item) => item.value === operation.topology,
    );
    const dimensionRanks =
      !multi &&
      ["torus", "mesh", "hypercube", "2dfullmesh"].includes(operation.topology);
    document.getElementById("num-gpus").readOnly = dimensionRanks;
    document.getElementById("dimension-ranks-hint").hidden = !dimensionRanks;
    document.querySelectorAll("[data-gpus]").forEach((button) => {
      button.disabled = dimensionRanks;
    });
    for (const option of document.getElementById("algorithm-select").options) {
      const disabled =
        (["nvls", "sharp", "collnetdirect", "collnetchain"].includes(
          option.value,
        ) &&
          (multi || operation.topology !== "switched")) ||
        (option.value === "hierarchical" &&
          !multi &&
          !["leafspine", "nvl72"].includes(operation.topology));
      if (option.disabled !== disabled) option.disabled = disabled;
    }
    document.getElementById("physical-topology-hint").textContent =
      topology?.hint || "选择实际物理连接方式；算法与拓扑分别设置。";
    document.getElementById("operation-hint").textContent =
      operation.collective === "alltoall"
        ? "非分层选项使用同一 AlltoAllInjector；CollNet 是该路径的别名。"
        : operation.algorithm === "hierarchical"
          ? "按节点分组，分别执行节点内与节点间通信。"
          : "NVLS / SHARP 需要单节点 NVSwitch；分层算法需要多节点或分组拓扑。";
    const source = this.source();
    for (const [id, sourceValue] of [
      ["collective-select", source.op.collective],
      ["algorithm-select", source.op.algorithm || "ring"],
      ["physical-topology", source.op.topology || "ring"],
    ]) {
      this.setDefaultLabel(
        document.getElementById(id),
        `模板默认 · ${sourceValue || "stencil"}`,
      );
    }
    const setting = (key) =>
      document.getElementById(`opt-${key}`)?.value ||
      this.preview?.settings?.[key] ||
      source.stack[key];
    for (const item of this.fields) {
      const input = document.getElementById(`opt-${item.key}`);
      const scope = this.applies(item, operation);
      let show = scope;
      if (this.schema.topology_fields.includes(item) && item.scope === "single")
        show = scope && (topology?.params || []).includes(item.key);
      if (item.when === "burst")
        show = scope && (setting("errorMode") || "independent") === "burst";
      if (item.when === "root")
        show = scope && ["broadcast", "reduce"].includes(operation.collective);
      if (["ncclring", "hierarchical"].includes(item.when))
        show = scope && operation.algorithm === item.when;
      if (item.when?.startsWith("so-"))
        show =
          scope &&
          document.getElementById("inter-topology").value ===
            item.when.slice(3);
      input.disabled = !scope;
      document.getElementById(`opt-${item.key}-field`).hidden = !show;
    }
    for (const group of this.schema.groups)
      document.getElementById(`options-${group.id}`).hidden =
        group.scope === "single" && multi;
    document.getElementById("topology-options").hidden =
      !this.topologyFields.some(
        (item) => !document.getElementById(`opt-${item.key}-field`).hidden,
      );
    document.getElementById("algorithm-options").hidden =
      !this.algorithmFields.some(
        (item) => !document.getElementById(`opt-${item.key}-field`).hidden,
      );
    const topologyName = multi
      ? document.getElementById("inter-topology").selectedOptions[0]
          ?.textContent
      : topology?.label;
    document.getElementById("topology-options-title").textContent =
      `拓扑参数 · ${topologyName || "当前网络"}`;
    document.getElementById("topology-options-description").textContent = multi
      ? "设置节点间网络的尺寸，例如 spine 数量或 torus 宽度。0 表示自动选择，通常可直接使用默认值。"
      : operation.topology === "leafspine"
        ? "Leaf 连接 GPU，Spine 连接各个 Leaf。这里设置两层交换机的数量；GPU 数须能被 Leaf 数整除。已填入可运行的默认值。"
        : operation.topology === "mesh"
          ? "设置网格的行数和列数。例如 2 行 × 4 列对应 8 个 GPU；修改维度后 GPU 总数会自动更新。"
          : operation.topology === "torus"
            ? "设置环绕网络的 X、Y、Z 维长度。例如 2 × 2 × 2 对应 8 个 GPU；修改维度后 GPU 总数会自动更新。"
            : operation.topology === "hypercube"
              ? "设置超立方体维数 d，GPU 总数为 2 的 d 次方。例如 3 维对应 8 个 GPU。"
              : "设置当前物理网络的交换机数量或维度。已有默认值，可以直接运行；修改前请查看参数旁的约束说明。";
    const algorithmDetail = ["broadcast", "reduce"].includes(
      operation.collective,
    )
      ? [
          "Root rank",
          "Root rank 从 0 开始编号：Broadcast 从这个 GPU 发出数据，Reduce 将归约结果交给这个 GPU。",
        ]
      : operation.algorithm === "ncclring"
        ? [
            "并行通道",
            "通道数控制并行 ring 的数量。0 表示按每节点 GPU 数自动设置。",
          ]
        : [
            "节点内算法",
            "Hierarchical 分别执行节点内和节点间通信。这里选择节点内阶段使用 Ring 或 Full mesh，物理连接方式由上方拓扑选择决定。",
          ];
    document.getElementById("algorithm-options-title").textContent =
      `算法参数 · ${algorithmDetail[0]}`;
    document.getElementById("algorithm-options-description").textContent =
      algorithmDetail[1];
  }

  apply(request) {
    const operation = this.operation();
    const op = { ...request.op_overrides },
      stack = request.stack_overrides,
      cli = request.cli_overrides;
    for (const item of this.fields) {
      if (
        this.applies(item, operation) &&
        (item.target === "stack" ||
          (item.target === "credits" && operation.stencil))
      )
        delete stack[item.key];
    }
    for (const [id, key] of [
      ["collective-select", "collective"],
      ["algorithm-select", "algorithm"],
      ["physical-topology", "topology"],
    ]) {
      const input = document.getElementById(id);
      if (!input.disabled && input.value) op[key] = input.value;
      else if (!input.disabled) delete op[key];
    }
    // An explicit uniform BER replaces inherited tier rates. Explicit tier
    // inputs below can still override it for a particular link class.
    if (document.getElementById("opt-ber").value !== "") {
      for (const key of [
        "berIntraNodeElectrical",
        "berIntraRackElectrical",
        "berInterRackOptical",
      ])
        stack[key] = "0";
      cli.berInterRackElectrical = "0";
    }
    for (const item of this.fields) {
      if (!this.applies(item, operation)) continue;
      const input = document.getElementById(`opt-${item.key}`);
      if (input.value === "") continue;
      if (
        this.schema.topology_fields.includes(item) &&
        document.getElementById(`opt-${item.key}-field`).hidden
      )
        continue;
      const target =
        item.target === "credits"
          ? operation.stencil
            ? "stack"
            : "cli"
          : item.target;
      (target === "stack" ? stack : cli)[item.key] = input.value;
    }
    return { ...request, op_overrides: op };
  }

  update(summary) {
    this.preview = summary;
    for (const item of this.fields) {
      const input = document.getElementById(`opt-${item.key}`);
      const value =
        item.key === "voqDropThreshold"
          ? summary.settings?.armedDropThreshold
          : (summary.settings?.[item.key] ??
            summary.topology_params?.[item.key] ??
            item.default);
      const label =
        item.choices?.find((choice) => choice.value === String(value))?.label ??
        value;
      if (item.choices)
        this.setDefaultLabel(input, `沿用 · ${label ?? "配置值"}`);
      else input.placeholder = `当前 ${value ?? "配置值"}`;
      document.getElementById(`opt-${item.key}-effective`).textContent =
        value == null ? "" : `当前：${label}`;
    }
    this.sync();
    const s = summary.settings || {},
      enabled = (key) => ["1", "true"].includes(String(s[key]));
    const controller =
      [enabled("dcqcn") ? "DCQCN" : "", enabled("rttcc") ? "RTTCC" : ""]
        .filter(Boolean)
        .join(" + ") || "关闭";
    const rates = [
      s.berIntraNodeElectrical,
      s.berIntraRackElectrical,
      s.berInterRackElectrical,
      s.berInterRackOptical,
    ].filter((value) => Number(value) > 0);
    const lossPercent = Number(
      (Number(s.packetLossRate || 0) * 100).toPrecision(6),
    );
    const errors = `BER ${Number(s.ber) > 0 ? s.ber : rates.length ? "分层设置" : "0"} · 丢包 ${lossPercent}%${s.errorMode === "burst" ? " · 突发" : ""}`;
    const rows = [
      [
        "发送控制",
        `Credit · ${Number(s.effectiveVcCredits).toLocaleString("en-US")} 分片 / VC`,
      ],
      ["错误注入", errors],
      [
        "FEC",
        enabled("fecEnabled")
          ? `RS(${s.fecN}, ${s.fecK}) · T=${s.fecT}`
          : "关闭",
      ],
      [
        "LLR 链路重传",
        enabled("llrEnabled")
          ? `${s.llrMode === "sack" ? "SACK" : "Go-back-N"}${s.llrRequested !== s.llrEnabled ? " · Fabric 强制启用" : ""}`
          : "关闭",
      ],
    ];
    if (summary.num_nodes > 1)
      rows.push(
        [
          "So 发送窗口",
          Number(s.soTxWindow) ? `${s.soTxWindow} 个在途分片` : "不限在途分片",
        ],
        [
          "拥塞控制 / PFC",
          `${controller} / ${Number(s.switchPfcMode) ? `模式 ${s.switchPfcMode}` : "关闭"}`,
        ],
      );
    if (Number(s.failedOpticalLink) >= 0)
      rows.push(["光链路故障", `移除链路 #${s.failedOpticalLink} · 重算路由`]);
    document.getElementById("effective-options").innerHTML = rows
      .map(
        ([key, value]) =>
          `<div class="effective-option"><span>${this.escape(key)}</span><strong>${this.escape(value)}</strong></div>`,
      )
      .join("");
  }

  clearErrors() {
    for (const key of [
      "ber",
      "packetLossRate",
      "berIntraNodeElectrical",
      "berIntraRackElectrical",
      "berInterRackElectrical",
      "berInterRackOptical",
      "burstArrivalRate",
    ])
      document.getElementById(`opt-${key}`).value = "0";
    document.getElementById("opt-errorMode").value = "independent";
  }

  changed() {
    this.sync();
    document
      .getElementById("simulation-form")
      .dispatchEvent(new Event("change", { bubbles: true }));
  }
}
