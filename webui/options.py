"""Native simulator choices and the fields shown by the experiment form.

Defaults and restrictions follow scratch/gpu-cluster-sim.cc and the topology
helpers. Keep this metadata shared by the API and the browser.
"""

from __future__ import annotations


COLLECTIVES = [
    {"value": "allreduce", "label": "AllReduce", "algorithms": ["auto", "ring", "tree", "fullmesh", "nvls", "sharp", "hierarchical", "ncclring"]},
    {"value": "allgather", "label": "AllGather", "algorithms": ["auto", "ring", "fullmesh", "nvls", "hierarchical"]},
    {"value": "reducescatter", "label": "ReduceScatter", "algorithms": ["ring", "fullmesh", "hierarchical"]},
    {"value": "alltoall", "label": "AllToAll", "algorithms": ["ring", "fullmesh", "hierarchical", "collnetdirect", "collnetchain"]},
    {"value": "broadcast", "label": "Broadcast", "algorithms": ["ring"]},
    {"value": "reduce", "label": "Reduce", "algorithms": ["ring"]},
]
ALGORITHMS = {
    "auto": "Auto · 按数据量选择", "ring": "Ring", "tree": "Tree",
    "fullmesh": "Full mesh", "nvls": "NVLS", "sharp": "SHARP",
    "hierarchical": "Hierarchical · 分层", "ncclring": "NCCL pipelined ring",
    "collnetdirect": "CollNet direct · AllToAll 别名",
    "collnetchain": "CollNet chain · AllToAll 别名",
}
TOPOLOGIES = [
    {"value": "ring", "label": "Ring · 环形", "params": []},
    {"value": "fullmesh", "label": "Full mesh · 全互联", "params": []},
    {"value": "switched", "label": "NVSwitch · 交换网络", "params": []},
    {"value": "fattree", "label": "Fat-tree", "params": ["fattreeRadix"]},
    {"value": "railfattree", "label": "Rail-optimized fat-tree", "params": ["railCount", "railNodesPerLeaf", "railSpineSwitches", "railCoreSwitches", "railLinksPerSpine"], "hint": "GPU 数须为 rail 数的整数倍；每 GPU 物理链路数须为 1。"},
    {"value": "leafspine", "label": "Leaf–spine", "params": ["leafSwitches", "spineSwitches"], "hint": "GPU 数须为 leaf 数的整数倍。"},
    {"value": "nvl72", "label": "NVL72 · 多交换平面", "params": ["nvl72SwitchPlanes", "nvl72GpusPerGroup"]},
    {"value": "torus", "label": "3D torus", "params": ["torusDimX", "torusDimY", "torusDimZ"], "hint": "GPU 数由 X × Y × Z 决定。"},
    {"value": "mesh", "label": "2D mesh", "params": ["meshRows", "meshCols"], "hint": "GPU 数由行数 × 列数决定。"},
    {"value": "hypercube", "label": "Hypercube · 超立方体", "params": ["hypercubeDims"], "hint": "GPU 数由 2 的维数次方决定。"},
    {"value": "2dfullmesh", "label": "2D full mesh", "params": ["rackRows", "rackCols"], "hint": "GPU 数由机架行数 × 列数决定。"},
    # Build2DFullMeshClos does not yet install complete multi-rack routes.
    # Do not offer it as a usable web preset until native routing is complete.
    {"value": "3levelhierarchical", "label": "Three-level hierarchical", "params": ["leafSwitches", "hier3MidSwitches", "spineSwitches"]},
    {"value": "dragonflyplus", "label": "Dragonfly+", "params": ["dflyGroups", "dflyRoutersPerGroup"]},
    {"value": "multiplane", "label": "Multi-plane", "params": ["multiPlaneCount"], "hint": "每 GPU 物理链路数须不少于交换平面数。"},
]
TOPOLOGY_DEFAULTS = {
    "fattreeRadix": 4, "railCount": 8, "railNodesPerLeaf": 32,
    "railSpineSwitches": 0, "railCoreSwitches": 0, "railLinksPerSpine": 0,
    "leafSwitches": 6, "spineSwitches": 3, "nvl72SwitchPlanes": 18,
    "nvl72GpusPerGroup": 0, "torusDimX": 4, "torusDimY": 4, "torusDimZ": 4,
    "meshRows": 4, "meshCols": 4, "hypercubeDims": 4, "rackRows": 8,
    "rackCols": 8, "numRacks": 1, "numSpineSwitchesUb": 0,
    "hier3MidSwitches": 2, "dflyGroups": 4, "dflyRoutersPerGroup": 2,
    "multiPlaneCount": 4,
}
PROFILE_SOURCED = {
    "protocolModel", "forceProtocol", "bandwidthGbps", "latencyNs", "numLanes",
    "linksPerGpu", "sprayChunkSize", "switchVoqDepth", "switchArbIntervalNs",
    "interNodeFabricType", "interNodeTopology", "interNodeBandwidthGbps", "interNodeLatencyNs",
    "fecN", "fecK", "fecT", "fecEnabled", "fecEncodeLatencyNs", "fecDecodeLatencyNs",
    "berIntraNodeElectrical", "berIntraRackElectrical", "berInterRackOptical",
    "llrEnabled", "llrMode", "vcCount", "vcCredits", "flowControl",
}
TIER_DEFAULTS = {"berIntraNodeElectrical": "1e-15", "berIntraRackElectrical": "1e-13", "berInterRackOptical": "1e-9"}


def field(key, label, *, target="cli", default=None, choices=None, scope=None, hint="", minimum=0, maximum=1000000000, kind="int", when=None):
    result = {"key": key, "label": label, "target": target, "hint": hint,
              "type": kind, "min": minimum, "max": maximum}
    if default is not None:
        result["default"] = default
    if choices is not None:
        result["choices"] = [{"value": str(value), "label": title} for value, title in choices]
    if scope:
        result["scope"] = scope
    if when:
        result["when"] = when
    return result


BOOL = [("1", "启用"), ("0", "关闭")]
TOPOLOGY_LABELS = {
    "fattreeRadix": "交换机 radix", "railCount": "Rail 数",
    "railNodesPerLeaf": "每 leaf 的节点数", "railSpineSwitches": "Rail spine 数（0=自动）",
    "railCoreSwitches": "Rail core 数（0=自动）", "railLinksPerSpine": "Leaf–spine 并行链路（0=自动）",
    "leafSwitches": "Leaf 数", "spineSwitches": "Spine 数", "nvl72SwitchPlanes": "NVSwitch 平面数",
    "nvl72GpusPerGroup": "每组 GPU（0=自动）", "torusDimX": "X 维长度", "torusDimY": "Y 维长度",
    "torusDimZ": "Z 维长度", "meshRows": "Mesh 行数", "meshCols": "Mesh 列数",
    "hypercubeDims": "超立方体维数", "rackRows": "机架行数", "rackCols": "机架列数",
    "numRacks": "机架数", "numSpineSwitchesUb": "Clos spine 数（0=自动）",
    "hier3MidSwitches": "中间层交换机数", "dflyGroups": "Dragonfly 分组数",
    "dflyRoutersPerGroup": "每组路由器数", "multiPlaneCount": "交换平面数",
}
TOPOLOGY_FIELDS = [
    field(key, TOPOLOGY_LABELS[key], default=default,
          minimum=0 if key in {"railSpineSwitches", "railCoreSwitches", "railLinksPerSpine", "nvl72GpusPerGroup", "numSpineSwitchesUb"} else 1,
          maximum=8 if key == "hypercubeDims" else 64, scope="single")
    for key, default in TOPOLOGY_DEFAULTS.items()
]
TOPOLOGY_FIELDS += [
    field("intraNodeTopology", "节点内物理拓扑", scope="multi", default="switched", choices=[("switched", "NVSwitch"), ("fullmesh", "Full mesh")]),
    field("intraNodeAlgorithm", "分层算法的节点内算法", scope="multi", default="fullmesh", choices=[("ring", "Ring"), ("fullmesh", "Full mesh")], when="hierarchical"),
    field("interNodeSpines", "节点间 spine 数（0=自动）", scope="multi", default=0, maximum=64, when="so-leafspine"),
    field("interNodeTorusWidth", "节点间 torus 宽度（0=自动）", scope="multi", default=0, maximum=32, when="so-torus2d"),
    field("interNodeDragonflyGroupSize", "Dragonfly 每组节点数（0=自动）", scope="multi", default=0, maximum=32, when="so-dragonfly"),
    field("rootRank", "Root rank", scope="collective", default=0, maximum=255, when="root"),
    field("ringChannels", "NCCL ring 通道数（0=自动）", scope="collective", default=0, maximum=64, when="ncclring"),
]

OPTION_GROUPS = [
    {"id": "hardware", "title": "分片与物理链路", "subtitle": "Lanes / 链路数 / 分片大小", "hint": "这些值会覆盖 profile 中的同名参数。物理链路数会影响交换拓扑的实际连线和带宽。", "fields": [
        field("linksPerGpu", "每 GPU 物理链路数", target="stack", default=1, minimum=1, maximum=64),
        field("numLanes", "每逻辑链路 lane 数", target="stack", default=1, minimum=1, maximum=64),
        field("sprayChunkSize", "Spray 分片大小 · bytes", target="stack", default=131072, minimum=1, maximum=67108864),
        field("bulkChunkSize", "Bulk 包大小上限 · bytes", default=8388608, minimum=1, maximum=67108864),
    ]},
    {"id": "flow", "title": "流控与拥塞控制", "subtitle": "Credit / 发送窗口 / PFC", "hint": "Credit 控制每个 VC 的可发分片数；So 窗口控制跨节点在途分片数。PFC 和拥塞控制可同时使用。", "fields": [
        field("vcCredits", "每 VC 信用额度 · 分片数", target="credits", minimum=1, maximum=1073741824),
        field("vcCount", "虚拟通道数量", target="stack", scope="stencil", default=4, minimum=1, maximum=255),
        field("vcBufferSizeBytes", "VC 缓冲区 · bytes", default=0, scope="collective", maximum=1073741824, hint="0 沿用信用额度；大于 0 时额度为缓冲区 / spray 分片大小。"),
        field("soTxWindow", "So 发送窗口 · 分片数", default=0, scope="multi", maximum=1000000, hint="0 不限在途分片；小窗口有助于控制 incast 突发。"),
        field("switchVoqDepth", "交换机 VOQ 容量 · 分片数", target="stack", default=10000, minimum=1, maximum=1000000),
        field("voqDropThreshold", "So VOQ 丢包阈值 · 分片数", scope="multi", default=8, maximum=999999, hint="0 关闭；RoCE 只有显式设置才开启尾丢包。须小于 VOQ 容量。"),
        field("switchPfcMode", "PFC 模式", scope="multi", default=0, choices=[("0", "关闭"), ("1", "模式 1 · 静态阈值"), ("2", "模式 2 · 动态阈值"), ("3", "模式 3 · 802.1Qbb 定时暂停")]),
        field("pfcPauseThreshold", "PFC 暂停阈值 · 分片数", scope="multi", default=64, minimum=1, maximum=999999, hint="须小于启用的丢包阈值。"),
        field("pfcResumeOffset", "PFC 恢复回差 · 分片数", scope="multi", default=8, maximum=999999),
        field("pfcPauseTime", "模式 3 暂停时间 · µs", scope="multi", default=0, kind="float", maximum=1000000),
        field("pfcHeadroomChunks", "PFC headroom · 分片数", scope="multi", default=0, maximum=1000000, hint="0 不限制；耗尽后会丢包并使用 LLR 恢复。"),
        field("dcqcn", "DCQCN · ECN / CNP 速率控制", scope="multi", default=0, choices=BOOL, hint="用于 RoCE 交换网络；需要队列达到 ECN 阈值后才能观察到降速。"),
        field("ecnThreshold", "ECN 标记阈值 · 分片数", scope="multi", default=8, maximum=999999),
        field("dcqcnTFastNs", "DCQCN 快速恢复间隔 · ns", scope="multi", default=0, hint="0 使用 50000 ns。"),
        field("dcqcnFastCycles", "DCQCN 快速恢复周期数", scope="multi", default=0, maximum=1000),
        field("dcqcnTAiNs", "DCQCN 加性增长间隔 · ns", scope="multi", default=0, hint="0 使用 100000 ns。"),
        field("dcqcnRai", "DCQCN Alpha 加性减量", scope="multi", default=0, kind="float", maximum=1),
        field("rttcc", "RTTCC · RTT 反馈速率控制", scope="multi", default=0, choices=BOOL, hint="支持跨节点 Ethernet 和 RoCE，不依赖 ECN。"),
        field("rttccTargetRttNs", "RTTCC 目标 RTT · ns", scope="multi", default=0, hint="0 使用 10000 ns；目标应高于无排队时 RTT。"),
        field("rttccProbeIntervalNs", "RTTCC 探测间隔 · ns", scope="multi", default=0, hint="0 使用 5000 ns。"),
        field("perQpCeilingGbps", "单 QP 速率上限 · Gbps", scope="multi", default=0, kind="float", maximum=100000, hint="0 不限速；通过发送端 pacer 实现。"),
    ]},
    {"id": "errors", "title": "错误注入", "subtitle": "BER / 丢包 / 突发错误", "hint": "BER 是每比特错误概率，丢包率是每包丢失概率。FEC 可以纠正误码，不能恢复随机丢包。", "fields": [
        field("errorMode", "误码模式", default="independent", choices=[("independent", "独立误码"), ("burst", "突发误码（关闭 FEC 后使用）")]),
        field("ber", "统一 BER", default=0, kind="float", maximum=1, hint="填写后覆盖配置中的分层 BER；下方显式填写的分层值仍优先。支持 1e-6。"),
        field("packetLossRate", "随机丢包率 · 0–1", default=0, kind="float", maximum=1, hint="例如 0.001 表示 0.1%；控制包也可能丢失。"),
        field("berIntraNodeElectrical", "节点内电链路 BER", target="stack", default=0, kind="float", maximum=1),
        field("berIntraRackElectrical", "机架内电链路 BER", target="stack", default=0, kind="float", maximum=1),
        field("berInterRackElectrical", "机架间电链路 BER", default=0, kind="float", maximum=1, scope="single"),
        field("berInterRackOptical", "机架间光链路 BER", target="stack", default=0, kind="float", maximum=1),
        field("codewordSize", "突发模式码字大小 · bytes", default=0, maximum=1048576, hint="0 使用 256 bytes。", when="burst"),
        field("burstLength", "平均突发长度 · 码字", default=1, minimum=1, maximum=1000, when="burst"),
        field("burstArrivalRate", "每码字突发到达概率", default=0, kind="float", maximum=1, when="burst"),
    ]},
    {"id": "reliability", "title": "纠错与重传", "subtitle": "FEC / Go-back-N / SACK", "hint": "FEC 和链路重传可同时使用。Ethernet、显式开启尾丢包的 RoCE 和有限 PFC headroom 会由仿真器强制开启 LLR。", "fields": [
        field("fecEnabled", "FEC 前向纠错", target="stack", default=1, choices=BOOL),
        field("fecN", "FEC 总符号数 N", target="stack", default=544, minimum=2, maximum=4096),
        field("fecK", "FEC 数据符号数 K", target="stack", default=514, minimum=1, maximum=4095),
        field("fecT", "FEC 纠错能力 T", target="stack", default=15, maximum=2047, hint="须满足 N > K，且 2T ≤ N − K。"),
        field("fecEncodeLatencyNs", "FEC 编码延迟 · ns", target="stack", default=50),
        field("fecDecodeLatencyNs", "FEC 解码延迟 · ns", target="stack", default=80),
        field("fecScope", "FEC 作用范围", default="all", choices=[("all", "全部链路"), ("optical", "仅光链路")]),
        field("llrEnabled", "LLR 链路重传", target="stack", default=0, choices=BOOL),
        field("llrMode", "重传方式", target="stack", default="gobackn", choices=[("gobackn", "Go-back-N · 回退重传"), ("sack", "SACK · 选择重传")]),
        field("llrRetryLimit", "每包重试次数上限", default=4294967295, maximum=4294967295, hint="4294967295 表示不设重试次数上限；任务仍受运行时间限制。"),
        field("llrRetryTimeout", "重传超时 · ns", default=0, hint="0 由仿真器决定。"),
        field("llrTimeoutRetransmit", "超时主动重传", default=0, choices=BOOL, hint="随机丢包下建议启用，避免 NACK 丢失后等待不结束。"),
        field("llrBufferSize", "重传缓冲区 · 包", default=1000, minimum=1, maximum=1000000),
        field("llrBufferSizeBytes", "重传缓冲区 · bytes", default=0, maximum=1073741824, hint="大于 0 时覆盖包数设置。"),
        field("llrOverflowPolicy", "重传缓冲区溢出策略", default="dropoldest", choices=[("dropoldest", "移除最旧记录"), ("dropnewest", "拒绝最新记录")]),
        field("soGbnStrict", "So 严格 Go-back-N 接收", scope="multi", default=0, choices=BOOL, hint="只接受顺序到达的数据，乱序数据触发缺口 NAK。"),
    ]},
    {"id": "optical", "title": "光链路故障", "subtitle": "Leaf–spine / 路由重算", "hint": "单节点 Leaf–spine 可将交换机间链路设为光链路，并在运行开始前移除一条链路。这里的“单节点”指使用统一拓扑构建器，仍可建多层交换网络。", "scope": "single", "fields": [
        field("interSwitchMedium", "交换机间链路介质", default="electrical", choices=[("electrical", "电链路"), ("optical", "光链路")]),
        field("failedOpticalLink", "移除的光链路编号", default=-1, minimum=-1, maximum=4095, hint="-1 不移除；Leaf–spine 编号从 0 开始，范围为 leaf × spine。"),
        field("failureAwareRouting", "按可用链路重算路由", default=0, choices=BOOL, hint="移除链路时会自动重算路由。是否仍连通以原生结果为准。"),
    ]},
]


def validate_value(key, value, rule):
    if "choices" in rule:
        choices = [item["value"] if isinstance(item, dict) else item for item in rule["choices"]]
        if set(choices) == {"0", "1"}:
            value = {"true": "1", "false": "0"}.get(value, value)
        if value not in choices:
            raise ValueError(f"{key} 可选值：{', '.join(choices)}")
    elif rule.get("type") == "bool":
        if value not in {"0", "1", "true", "false"}:
            raise ValueError(f"{key} 需要 0 或 1。")
    else:
        import math
        import re

        if rule.get("type", "int") == "int" and not re.fullmatch(r"-?\d+", value):
            raise ValueError(f"{key} 需要整数。")
        try:
            number = float(value)
        except ValueError:
            raise ValueError(f"{key} 需要数值。") from None
        if not math.isfinite(number) or not rule["min"] <= number <= rule["max"]:
            raise ValueError(f"{key} 范围为 {rule['min']}–{rule['max']}。")


def validate_configuration(op, resolved, cli, request):
    """Return actual rank count, effective settings and compatibility notes."""
    stencil = "collective" not in op
    multi = request.num_nodes > 1
    topology = op.get("topology", "ring")
    if topology not in {item["value"] for item in TOPOLOGIES}:
        raise ValueError("topology 不是支持的物理拓扑。")
    # Stack values beat CLI for fields sourced from ProtocolProfile. Credits
    # are the exception: ApplyBundle owns them only for transfer stencils.
    defaults = {item["key"]: item["default"] for group in OPTION_GROUPS for item in group["fields"] if "default" in item}
    defaults.update(TOPOLOGY_DEFAULTS)
    defaults.update(vcCredits=64 if stencil else 104857600, sprayChunkSize=131072, linksPerGpu=1,
                    intraNodeTopology="switched", intraNodeAlgorithm="fullmesh")
    settings = {**defaults, **cli, **{key: value for key, value in resolved.items() if key in PROFILE_SOURCED}}
    # Build() consumes the bundle even for an inline collective. Validate its
    # raw fields before replacing the VC values with the inline defaults.
    for group in OPTION_GROUPS:
        for item in group["fields"]:
            if item["key"] in resolved:
                validate_value(item["key"], str(resolved[item["key"]]), item)
    if not stencil:
        settings["vcCredits"] = cli.get("vcCredits", 104857600)
        settings["vcCount"] = 1
    else:
        settings["vcCredits"] = resolved.get("vcCredits", 64)
        settings["vcCount"] = resolved.get("vcCount", 4)
        settings["llrEnabled"] = resolved.get("llrEnabled", "0")
    for key in TIER_DEFAULTS:
        settings[key] = resolved.get(key, cli.get(key, "0"))
    if all(float(settings[key]) == 0 for key in [*TIER_DEFAULTS, "berInterRackElectrical"]):
        for key, default in TIER_DEFAULTS.items():
            settings[key] = resolved.get(key, default)
    settings["fecEnabled"] = resolved.get("fecEnabled", cli.get("fecEnabled", "1"))
    settings["interNodeTopology"] = resolved.get("interNodeTopology", cli.get("interNodeTopology", "fullmesh"))
    settings["interNodeFabricType"] = resolved.get("interNodeFabricType", "RoCE")
    for group in OPTION_GROUPS:
        for item in group["fields"]:
            if item["key"] in settings:
                validate_value(item["key"], str(settings[item["key"]]), item)
    # The bundle is always built, even when a collective uses the inline FEC
    # object. Validate it separately from the native global N=K=0 default.
    bundle_fec = [int(resolved.get(key, default)) for key, default in [("fecN", 544), ("fecK", 514), ("fecT", 15)]]
    if bundle_fec[0] <= bundle_fec[1] or 2 * bundle_fec[2] > bundle_fec[0] - bundle_fec[1]:
        raise ValueError("FEC 参数需要 N > K，且 2T ≤ N − K。即使关闭 FEC，参数也需满足原生模型约束。")
    if stencil:
        for key, default in [("fecN", 544), ("fecK", 514), ("fecT", 15), ("fecEncodeLatencyNs", 50), ("fecDecodeLatencyNs", 80)]:
            settings[key] = resolved.get(key, default)
    else:
        for key in ["fecN", "fecK", "fecT"]:
            settings[key] = resolved.get(key, cli.get(key, 0))
        for key, cli_key, default in [("fecEncodeLatencyNs", "fecEncodeLatency", 50), ("fecDecodeLatencyNs", "fecDecodeLatency", 80)]:
            settings[key] = resolved.get(key, cli.get(cli_key, default))
        if not int(settings["fecN"]) or not int(settings["fecK"]):
            settings["fecEnabled"] = "0"
    for key, value in resolved.items():
        if key == "flowControl" and value != "credit":
            raise ValueError("flowControl 当前仅实现 credit；window/rate 是预留接口。跨节点窗口请使用 soTxWindow，速率控制请选择 DCQCN / RTTCC。")
    settings = {key: str(value) for key, value in settings.items()}
    for group in OPTION_GROUPS:
        for item in group["fields"]:
            if {choice["value"] for choice in item.get("choices", [])} == {"0", "1"}:
                key = item["key"]
                settings[key] = {"true": "1", "false": "0"}.get(settings[key], settings[key])
    integer = lambda key: int(settings[key])
    enabled = lambda key: settings.get(key, "0") in {"1", "true"}
    ranks = request.num_nodes * request.gpus_per_node if multi else request.num_gpus
    notices = []
    if not stencil and (not integer("fecN") or not integer("fecK")):
        notices.append("当前集合通信没有完整的 FEC 码字参数，FEC 未启用；可在纠错与重传中填写 N、K、T。")
    if stencil:
        for key in ["vcCredits", "llrEnabled"]:
            if key in cli and cli[key] != settings[key]:
                notices.append(f"传输 stencil 的 {key} 使用 profile / [stack] 值 {settings[key]}，不会使用附加 CLI 的 {cli[key]}。")
    for group in OPTION_GROUPS:
        for item in group["fields"]:
            key = item["key"]
            if item["target"] == "cli" and key in resolved and key not in PROFILE_SOURCED:
                notices.append(f"[stack] / profile 中的 {key} 不会传给原生 CLI；请在表单或附加 CLI 中设置。")
    sourced_cli = {"fecEncodeLatency": "fecEncodeLatencyNs", "fecDecodeLatency": "fecDecodeLatencyNs"}
    for key in ["fecN", "fecK", "fecT", "fecEnabled", "llrEnabled", "llrMode", "berIntraNodeElectrical", "berIntraRackElectrical", "berInterRackOptical", *sourced_cli]:
        profile_key = sourced_cli.get(key, key)
        if key in cli and profile_key in resolved and cli[key] != str(resolved[profile_key]):
            notices.append(f"{profile_key} 使用配置 / profile 的值 {resolved[profile_key]}，覆盖附加 CLI 的 {cli[key]}。")
    topology_params = {key: int(cli.get(key, value)) for key, value in TOPOLOGY_DEFAULTS.items()}
    if not multi:
        factors = {"torus": ["torusDimX", "torusDimY", "torusDimZ"], "mesh": ["meshRows", "meshCols"],
                   "2dfullmesh": ["rackRows", "rackCols"], "2dfullmeshclos": ["rackRows", "rackCols", "numRacks"]}
        if topology in factors:
            import math
            ranks = math.prod(topology_params[key] for key in factors[topology])
        if topology == "hypercube":
            ranks = 2 ** topology_params["hypercubeDims"]
        if not 2 <= ranks <= 256:
            raise ValueError(f"拓扑维度生成 {ranks} 个 GPU；网页支持 2–256 个。")
        if ranks != request.num_gpus:
            notices.append(f"拓扑维度决定实际 GPU 数为 {ranks}。")
        if topology in {"leafspine", "3levelhierarchical", "dragonflyplus"}:
            leaves = topology_params["dflyGroups"] * topology_params["dflyRoutersPerGroup"] if topology == "dragonflyplus" else topology_params["leafSwitches"]
            if ranks % leaves or ranks < leaves:
                raise ValueError(f"{topology} 要求 GPU 数 {ranks} 能被 leaf / 路由器数 {leaves} 整除。")
        if topology == "3levelhierarchical" and topology_params["leafSwitches"] % topology_params["hier3MidSwitches"]:
            raise ValueError("Leaf 数需要能被中间层交换机数整除。")
        if topology == "railfattree" and (ranks % topology_params["railCount"] or integer("linksPerGpu") != 1):
            raise ValueError("Rail fat-tree 要求 GPU 数能被 rail 数整除，并设置 linksPerGpu = 1。")
        if topology == "multiplane" and integer("linksPerGpu") < topology_params["multiPlaneCount"]:
            raise ValueError("Multi-plane 要求 linksPerGpu 不小于平面数。")
        group = topology_params["nvl72GpusPerGroup"] or max(1, ranks // 6)
        if topology == "nvl72" and ranks % group:
            raise ValueError("NVL72 要求 GPU 数能被每组 GPU 数整除。")
        if topology == "fattree" and topology_params["fattreeRadix"] % 2:
            raise ValueError("Fat-tree radix 需要是偶数。")
    if not stencil:
        collective = op["collective"]
        definition = next((item for item in COLLECTIVES if item["value"] == collective), None)
        algorithm = op.get("algorithm", "ring")
        if definition is None or algorithm not in definition["algorithms"]:
            raise ValueError(f"{collective} 不支持算法 {algorithm}；请选择该操作提供的算法。")
        if algorithm in {"nvls", "sharp", "collnetdirect", "collnetchain"} and (topology != "switched" or multi):
            raise ValueError(f"{algorithm} 需要单节点 NVSwitch（topology = switched）。")
        if algorithm == "hierarchical" and not multi and topology not in {"leafspine", "nvl72"}:
            raise ValueError("Hierarchical 需要多节点场景，或 Leaf–spine / NVL72 分组拓扑。")
        if collective == "alltoall" and algorithm != "hierarchical":
            notices.append("非分层 AllToAll 使用同一 AlltoAllInjector；CollNet 选项是该路径的别名。")
    if integer("fecN") and integer("fecK") and (integer("fecN") <= integer("fecK") or 2 * integer("fecT") > integer("fecN") - integer("fecK")):
        raise ValueError("FEC 参数需要 N > K，且 2T ≤ N − K。即使关闭 FEC，参数也需满足原生模型约束。")
    fabric = settings["interNodeFabricType"]
    so_topology = settings["interNodeTopology"]
    switched_so = so_topology in {"host", "tor", "rail", "leafspine"}
    drop = int(cli.get("voqDropThreshold", 8 if fabric == "ETH" else 0)) if multi else 0
    pfc = integer("switchPfcMode") if multi else 0
    forced_llr = multi and (fabric == "ETH" or (fabric == "RoCE" and drop > 0) or (pfc > 0 and integer("pfcHeadroomChunks") > 0))
    settings["llrRequested"] = settings["llrEnabled"]
    if forced_llr:
        settings["llrEnabled"] = "1"
        if not enabled("llrRequested"):
            notices.append("该 Fabric / 丢包设置会由原生仿真器强制开启 LLR。")
    settings["armedDropThreshold"] = str(drop)
    if multi and enabled("dcqcn") and (fabric != "RoCE" or not switched_so):
        raise ValueError("DCQCN 需要 RoCE 和含交换机的节点间拓扑；Ethernet 可选择 RTTCC。")
    if multi and fabric in {"ETH", "RoCE"}:
        if drop and drop >= integer("switchVoqDepth"):
            raise ValueError("VOQ 丢包阈值须小于交换机 VOQ 容量；0 表示关闭丢包。")
        if pfc and drop and integer("pfcPauseThreshold") >= drop:
            raise ValueError("PFC 暂停阈值须小于已启用的 VOQ 丢包阈值。")
        if pfc and not switched_so:
            notices.append("当前节点间拓扑没有交换机，PFC 不会触发；可选 Host / ToR / Rail / Leaf–spine。")
        if pfc == 3 and float(settings["pfcPauseTime"]) <= 0:
            raise ValueError("PFC 模式 3 需要大于 0 的暂停时间（µs）。")
        if enabled("dcqcn") and drop and integer("ecnThreshold") >= drop:
            raise ValueError("开启 DCQCN 和尾丢包时，ECN 标记阈值须小于丢包阈值。")
    for key, divisor_key in [("interNodeTorusWidth", "torus2d"), ("interNodeDragonflyGroupSize", "dragonfly")]:
        count = int(cli.get(key, 0))
        if multi and so_topology == divisor_key and count and request.num_nodes % count:
            raise ValueError(f"{key} 需要整除节点数。")
    if settings["errorMode"] == "burst":
        if enabled("fecEnabled"):
            raise ValueError("突发误码实验请关闭 FEC；当前 FEC 接收路径按独立 BER 解码，不经过突发模型。")
        if float(settings["ber"]) <= 0:
            raise ValueError("突发误码模式需要大于 0 的统一 BER，以启用原生突发错误检测。")
    if float(settings["packetLossRate"]) > 0 or float(settings["ber"]) > 0:
        if not enabled("llrEnabled"):
            notices.append("已注入错误而未启用 LLR；未被 FEC 纠正的丢失可能使操作无法完成。")
        elif not enabled("llrTimeoutRetransmit"):
            notices.append("随机错误可能使 NACK 丢失；可启用“超时主动重传”来恢复这类传输。")
    if multi:
        if integer("failedOpticalLink") >= 0 or enabled("failureAwareRouting"):
            raise ValueError("当前光链路故障选项用于单节点统一拓扑构建器；请选择单节点 Leaf–spine。")
        if fabric == "NVLink" and (pfc or enabled("dcqcn") or enabled("rttcc")):
            notices.append("跨节点流控实验建议选择 Ethernet 或 RoCE，并在原生结果中核对控制事件。")
    elif integer("failedOpticalLink") >= 0:
        if topology != "leafspine" or settings["interSwitchMedium"] != "optical":
            raise ValueError("网页的单光链路故障实验需要 Leaf–spine 拓扑，并将交换机间介质设为光链路。")
        count = topology_params["leafSwitches"] * topology_params["spineSwitches"]
        if integer("failedOpticalLink") >= count:
            raise ValueError(f"光链路编号需在 0–{count - 1} 之间。")
    if not multi and (any(enabled(key) for key in ["dcqcn", "rttcc"]) or integer("soTxWindow")):
        notices.append("So 发送窗口和拥塞控制仅作用于跨节点流量，单节点场景中不会生效。")
    settings["effectiveVcCredits"] = settings["vcCredits"]
    if not stencil and integer("vcBufferSizeBytes"):
        settings["effectiveVcCredits"] = str(max(1, integer("vcBufferSizeBytes") // integer("sprayChunkSize")))
    return ranks, settings, notices, topology_params
