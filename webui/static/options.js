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
        <summary><svg><use href="#i-sliders"/></svg><span class="options-title" data-i18n="${this.escape(group.title)}">${this.escape(t(group.title))}</span><span data-i18n="${this.escape(group.subtitle)}">${this.escape(t(group.subtitle))}</span></summary>
        <div class="advanced-content"><p class="field-hint" data-i18n="${this.escape(group.hint)}">${this.escape(t(group.hint))}</p>
          ${group.id === "errors" ? '<div class="option-actions"><button type="button" class="secondary-button" id="clear-errors" data-i18n="No errors">No errors</button><button type="button" class="secondary-button" id="loss-sack-preset" data-i18n="0.1% loss + SACK">0.1% loss + SACK</button></div>' : ""}
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
      ? `<select id="${id}" data-native-key="${item.key}"><option value="">${e(t("Use configuration value"))}</option>${item.choices.map((choice) => `<option value="${e(choice.value)}">${e(t(choice.label))}</option>`).join("")}</select>`
      : `<input id="${id}" data-native-key="${item.key}" type="number" min="${item.min}" max="${item.max}" step="${item.type === "float" ? "any" : "1"}" placeholder="${e(t("Use configuration value"))}" aria-describedby="${id}-effective ${id}-hint"/>`;
    return `<div class="field" id="${id}-field"><label for="${id}" data-i18n="${e(item.label)}">${e(t(item.label))}</label>${input}<small class="option-effective" id="${id}-effective"></small>${item.hint ? `<p class="field-hint" id="${id}-hint" data-i18n="${e(item.hint)}">${e(t(item.hint))}</p>` : `<span id="${id}-hint"></span>`}</div>`;
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

  setChoices(id, choices, value, inherited = t("Use configuration value")) {
    const e = this.escape,
      input = document.getElementById(id);
    let markup =
      `<option value="">${e(inherited)}</option>` +
      choices
        .map(
          (item) =>
            `<option value="${e(item.value)}">${e(t(item.label))}</option>`,
        )
        .join("");
    if (value && !choices.some((item) => item.value === value)) {
      markup += `<option value="${e(value)}">${e(value)}${e(t(" · check compatibility"))}</option>`;
    }
    const signature = JSON.stringify([
      I18n.language,
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
      t(topology?.hint) ||
      t(
        "Choose the physical links between devices. Select the collective algorithm separately.",
      );
    document.getElementById("operation-hint").textContent =
      operation.collective === "alltoall"
        ? t(
            "Non-hierarchical AllToAll choices share AlltoAllInjector. The CollNet names are aliases for that path.",
          )
        : operation.algorithm === "hierarchical"
          ? t("Groups ranks by node and runs intra-node and inter-node phases.")
          : t(
              "NVLS / SHARP need single-node NVSwitch. Hierarchical algorithms need multiple nodes or grouped topologies.",
            );
    const source = this.source();
    for (const [id, sourceValue] of [
      ["collective-select", source.op.collective],
      ["algorithm-select", source.op.algorithm || "ring"],
      ["physical-topology", source.op.topology || "ring"],
    ]) {
      this.setDefaultLabel(
        document.getElementById(id),
        t("Template default · {value0}", { value0: sourceValue || "stencil" }),
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
    document.getElementById("topology-options-title").textContent = t(
      "Topology parameters · {value0}",
      { value0: t(topologyName) || t("current network") },
    );
    document.getElementById("topology-options-description").textContent = multi
      ? t(
          "Set the inter-node network size, such as spine count or torus width. 0 selects it automatically. The defaults usually suffice.",
        )
      : operation.topology === "leafspine"
        ? t(
            "Leaf switches connect GPUs; spine switches connect the leaves. Set the switch count in each tier. GPU count must be divisible by leaf count. Valid defaults are provided.",
          )
        : operation.topology === "mesh"
          ? t(
              "Set mesh rows and columns. For example, 2 rows × 4 columns gives 8 GPUs. Changing the dimensions updates the GPU count.",
            )
          : operation.topology === "torus"
            ? t(
                "Set torus dimensions X, Y, and Z. For example, 2 × 2 × 2 gives 8 GPUs. Changing the dimensions updates the GPU count.",
              )
            : operation.topology === "hypercube"
              ? t(
                  "Set hypercube dimension d. The GPU count is 2 to the power d; 3 dimensions gives 8 GPUs.",
                )
              : t(
                  "Set switch counts or dimensions for this network. The defaults are ready to run. Check the constraints beside each field before changing them.",
                );
    const algorithmDetail = ["broadcast", "reduce"].includes(
      operation.collective,
    )
      ? [
          t("Root rank"),
          t(
            "Root ranks start at 0. Broadcast sends data from this GPU; Reduce delivers the result to this GPU.",
          ),
        ]
      : operation.algorithm === "ncclring"
        ? [
            t("Parallel channels"),
            t(
              "Channels set the number of parallel rings. 0 selects the count from GPUs per node.",
            ),
          ]
        : [
            t("Intra-node algorithm"),
            t(
              "Hierarchical operations have intra-node and inter-node phases. Choose Ring or Full mesh for the intra-node phase here. The topology selectors above set the physical links.",
            ),
          ];
    document.getElementById("algorithm-options-title").textContent = t(
      "Algorithm parameters · {value0}",
      { value0: algorithmDetail[0] },
    );
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
        this.setDefaultLabel(
          input,
          t("Inherit · {value0}", {
            value0: t(label ?? "configuration value"),
          }),
        );
      else
        input.placeholder = t("Current {value0}", {
          value0: value ?? t("configuration value"),
        });
      document.getElementById(`opt-${item.key}-effective`).textContent =
        value == null ? "" : t("Current: {value0}", { value0: t(label) });
    }
    this.sync();
    const s = summary.settings || {},
      enabled = (key) => ["1", "true"].includes(String(s[key]));
    const controller =
      [enabled("dcqcn") ? "DCQCN" : "", enabled("rttcc") ? "RTTCC" : ""]
        .filter(Boolean)
        .join(" + ") || t("Disabled");
    const rates = [
      s.berIntraNodeElectrical,
      s.berIntraRackElectrical,
      s.berInterRackElectrical,
      s.berInterRackOptical,
    ].filter((value) => Number(value) > 0);
    const lossPercent = Number(
      (Number(s.packetLossRate || 0) * 100).toPrecision(6),
    );
    const errors = t("BER {value0} · loss {value1}%{value2}", {
      value0:
        Number(s.ber) > 0 ? s.ber : rates.length ? t("tiered rates") : "0",
      value1: lossPercent,
      value2: s.errorMode === "burst" ? t(" · burst") : "",
    });
    const rows = [
      [
        t("Send control"),
        t("Credits · {value0} chunks / VC", {
          value0: Number(s.effectiveVcCredits).toLocaleString("en-US"),
        }),
      ],
      [t("Error injection"), errors],
      [
        "FEC",
        enabled("fecEnabled")
          ? `RS(${s.fecN}, ${s.fecK}) · T=${s.fecT}`
          : t("Disabled"),
      ],
      [
        t("Link-level retry (LLR)"),
        enabled("llrEnabled")
          ? `${s.llrMode === "sack" ? "SACK" : "Go-back-N"}${s.llrRequested !== s.llrEnabled ? t(" · forced by fabric") : ""}`
          : t("Disabled"),
      ],
    ];
    if (summary.num_nodes > 1)
      rows.push(
        [
          t("So TX window"),
          Number(s.soTxWindow)
            ? t("{value0} in-flight chunks", { value0: s.soTxWindow })
            : t("Unlimited in-flight chunks"),
        ],
        [
          t("Congestion control / PFC"),
          `${controller} / ${Number(s.switchPfcMode) ? t("Mode {value0}", { value0: s.switchPfcMode }) : t("Disabled")}`,
        ],
      );
    if (Number(s.failedOpticalLink) >= 0)
      rows.push([
        t("Optical link failure"),
        t("Remove link #{value0} · recompute routes", {
          value0: s.failedOpticalLink,
        }),
      ]);
    document.getElementById("effective-options").innerHTML = rows
      .map(
        ([key, value]) =>
          `<div class="effective-option"><span>${this.escape(key)}</span><strong>${this.escape(value)}</strong></div>`,
      )
      .join("");
  }

  relocalize() {
    for (const item of this.fields) {
      const input = document.getElementById(`opt-${item.key}`);
      for (const choice of item.choices || []) {
        const option = [...input.options].find(
          (option) => option.value === choice.value,
        );
        if (option) option.textContent = t(choice.label);
      }
    }
    for (const [id, choices] of [
      ["collective-select", this.schema.collectives],
      ["physical-topology", this.schema.topologies],
    ])
      this.setChoices(id, choices, document.getElementById(id).value);
    this.updateAlgorithms();
    if (this.preview) this.update(this.preview);
    else this.sync();
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
