"""Repository inputs and validated, reproducible simulation requests."""

from __future__ import annotations

import math
import re
from pathlib import Path
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, model_validator


class RunRequest(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str = Field(default="", max_length=80)
    config_id: str = Field(default="h200-ring-allreduce.cfg", pattern=r"^[\w-]+\.cfg$")
    config_text: str | None = Field(default=None, max_length=65536)
    profile_text: str | None = Field(default=None, max_length=65536)
    num_gpus: int = Field(default=8, ge=2, le=256, strict=True)
    num_nodes: int = Field(default=1, ge=1, le=32, strict=True)
    gpus_per_node: int = Field(default=4, ge=1, le=64, strict=True)
    data_size: int = Field(default=1048576, ge=1, le=1073741824, strict=True)
    seed: int = Field(default=1, ge=1, le=2147483647, strict=True)
    timeout: int = Field(default=120, ge=1, le=300, strict=True)
    stack_overrides: dict[str, str | int | float | bool] = Field(default_factory=dict)
    cli_overrides: dict[str, str | int | float | bool] = Field(default_factory=dict)

    @model_validator(mode="after")
    def check_scale(self):
        if self.num_nodes > 1 and not 2 <= self.num_nodes * self.gpus_per_node <= 256:
            raise ValueError("多节点场景的总 rank 数需在 2–256 之间。")
        return self


# These map to actual cmd.AddValue entries, rather than arbitrary shell arguments.
# Profile-sourced hardware is edited in [stack], not supplied as ineffective CLI overrides.
CLI_FIELDS: dict[str, dict[str, Any]] = {
    "ber": {"type": "float", "min": 0, "max": 1},
    "packetLossRate": {"type": "float", "min": 0, "max": 1},
    "fecEnabled": {"type": "bool"},
    "fecScope": {"choices": ["all", "optical"]},
    "llrRetryLimit": {"type": "int", "min": 0, "max": 100000},
    "llrRetryTimeout": {"type": "int", "min": 0, "max": 1000000000},
    "llrTimeoutRetransmit": {"type": "bool"},
    "vcCredits": {"type": "int", "min": 1, "max": 1073741824},
    "vcBufferSizeBytes": {"type": "int", "min": 0, "max": 1073741824},
    "bulkChunkSize": {"type": "int", "min": 1, "max": 67108864},
    "switchVoqDepth": {"type": "int", "min": 1, "max": 100000},
    "voqDropThreshold": {"type": "int", "min": 0, "max": 99999},
    "ecnThreshold": {"type": "int", "min": 0, "max": 99999},
    "switchPfcMode": {"type": "int", "min": 0, "max": 3},
    "pfcPauseThreshold": {"type": "int", "min": 1, "max": 99999},
    "pfcResumeOffset": {"type": "int", "min": 0, "max": 99999},
    "pfcPauseTime": {"type": "float", "min": 0, "max": 1000000},
    "dcqcn": {"type": "bool"},
    "rttcc": {"type": "bool"},
    "rttccTargetRttNs": {"type": "int", "min": 1, "max": 1000000000},
    "rttccProbeIntervalNs": {"type": "int", "min": 1, "max": 1000000000},
    "interNodeTopology": {"choices": ["host", "tor", "rail", "leafspine", "fullmesh", "ring", "torus2d", "dragonfly"]},
    "interNodeSpines": {"type": "int", "min": 0, "max": 64},
    "interNodeTorusWidth": {"type": "int", "min": 0, "max": 32},
    "interNodeDragonflyGroupSize": {"type": "int", "min": 0, "max": 32},
    "intraNodeTopology": {"choices": ["switched", "fullmesh"]},
    "intraNodeAlgorithm": {"choices": ["ring", "fullmesh"]},
    "ringChannels": {"type": "int", "min": 0, "max": 64},
    "rootRank": {"type": "int", "min": 0, "max": 255},
    "bgP2pEnabled": {"type": "bool"},
    "bgP2pSrc": {"type": "int", "min": 0, "max": 255},
    "bgP2pDst": {"type": "int", "min": 0, "max": 255},
    "bgP2pSizeBytes": {"type": "int", "min": 1, "max": 67108864},
    "bgP2pIntervalNs": {"type": "int", "min": 0, "max": 1000000000},
    "bgP2pStartTimeNs": {"type": "int", "min": 0, "max": 1000000000},
    "randTraffic": {"type": "bool"},
    "randInjectionRate": {"type": "float", "min": 0.001, "max": 1},
    "randMsgBytes": {"type": "int", "min": 1, "max": 16777216},
    "randWarmupNs": {"type": "int", "min": 0, "max": 10000000},
    "randMeasureNs": {"type": "int", "min": 1, "max": 10000000},
}

PRESETS = {
    "h200-ring-allreduce.cfg": ("Ring AllReduce", "单节点归约，H200 / NVLink", "collective", {}),
    "h200-tree-allreduce.cfg": ("Tree AllReduce", "树形归约，NVSwitch 交换网络", "collective", {}),
    "h200-nvls-allgather.cfg": ("NVLS AllGather", "NVSwitch 上的 AllGather", "collective", {}),
    "h200-request-response.cfg": ("请求 / 响应", "4 KiB 请求送达后返回 64 KiB 响应", "stencil", {"data_size": 4096}),
    "eth-hierarchical.cfg": ("多节点 AllReduce", "节点内 NVLink，节点间 Ethernet", "multinode", {"num_nodes": 2, "gpus_per_node": 4}),
    "p2p-single.cfg": ("点对点传输", "同节点 GPU 0 → GPU 1", "stencil", {}),
    "p2p-eth.cfg": ("Ethernet 点对点", "两个节点之间的 DATA 传输", "multinode", {"num_nodes": 2, "gpus_per_node": 2, "stack_overrides": {"interNodeFabricType": "ETH", "interNodeTopology": "host"}}),
    "p2p-eth-incast.cfg": ("Ethernet Incast", "两个发送端向同一目标发送", "multinode", {"num_nodes": 2, "gpus_per_node": 2, "stack_overrides": {"interNodeFabricType": "ETH", "interNodeTopology": "host"}}),
    "p2p-roce.cfg": ("RoCE 点对点", "节点间 RoCE 传输", "multinode", {"num_nodes": 2, "gpus_per_node": 2, "stack_overrides": {"interNodeFabricType": "RoCE", "interNodeTopology": "host"}}),
    "p2p-roce-lossy.cfg": ("RoCE · Go-back-N", "启用链路重试的 RoCE 场景", "multinode", {"num_nodes": 2, "gpus_per_node": 2, "stack_overrides": {"interNodeFabricType": "RoCE", "interNodeTopology": "host"}}),
    "p2p-roce-lossy-sack.cfg": ("RoCE · SACK", "选择重传的 RoCE 场景", "multinode", {"num_nodes": 2, "gpus_per_node": 2, "stack_overrides": {"interNodeFabricType": "RoCE", "interNodeTopology": "host"}}),
}


def parse_values(text: str, sections: bool = False) -> dict[str, dict[str, str]]:
    """Match the native parser's case-sensitive keys and inline # comments."""
    result: dict[str, dict[str, str]] = {"": {}}
    section = ""
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith(";"):
            continue
        if sections and line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            if section not in {"stack", "op"}:
                raise ValueError(f"配置第 {number} 行：只支持 [stack] 和 [op]。")
            result.setdefault(section, {})
            continue
        if "=" not in line:
            raise ValueError(f"第 {number} 行需要 key = value。")
        key, value = [s.strip() for s in line.split("=", 1)]
        if not re.fullmatch(r"[A-Za-z_][\w.]*", key) or not value or "\x00" in value:
            raise ValueError(f"第 {number} 行的键或值无效。")
        if key in result[section]:
            raise ValueError(f"第 {number} 行重复定义 {key}。")
        result[section][key] = value
    return result


def scalar(value: Any) -> str:
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError("参数必须为有限数值。")
    text = str(value)
    if not re.fullmatch(r"[A-Za-z0-9_:./+\-]+", text) or len(text) > 128:
        raise ValueError("覆盖值只能是一个数值或参数标识符。")
    return text


def validate_cli(values: dict[str, Any]) -> dict[str, str]:
    result = {}
    for key, value in values.items():
        if key not in CLI_FIELDS:
            raise ValueError(f"不支持 CLI 参数 {key}；硬件和协议参数请在配置中编辑。")
        field = CLI_FIELDS[key]
        text = scalar(value)
        if "choices" in field:
            if text not in field["choices"]:
                raise ValueError(f"{key} 可选值：{', '.join(field['choices'])}")
        elif field["type"] == "bool":
            if text not in {"0", "1", "true", "false"}:
                raise ValueError(f"{key} 需要 0 或 1。")
        else:
            if field["type"] == "int" and not re.fullmatch(r"\d+", text):
                raise ValueError(f"{key} 需要非负整数。")
            try:
                number = float(text)
            except ValueError:
                raise ValueError(f"{key} 需要数值。") from None
            if not field["min"] <= number <= field["max"]:
                raise ValueError(f"{key} 范围为 {field['min']}–{field['max']}。")
        result[key] = text
    return result


class Catalog:
    def __init__(self, repo: Path):
        self.repo = repo.resolve()
        self.config_dir = self.repo/"configs/protocol_configs"
        self.profile_dir = self.repo/"configs/protocol_profiles"

    def config(self, name: str) -> dict[str, Any]:
        if not re.fullmatch(r"[\w-]+\.cfg", name):
            raise ValueError("配置文件名无效。")
        path = (self.config_dir/name).resolve()
        if path.parent != self.config_dir.resolve() or not path.is_file():
            raise ValueError("找不到这份仓库配置。")
        text = path.read_text()
        parsed = parse_values(text, sections=True)
        profile = parsed.get("stack", {}).get("profile", "")
        profile_path = (self.repo/profile).resolve()
        if profile_path.parent != self.profile_dir.resolve() or not profile_path.is_file():
            raise ValueError("profile 必须来自仓库的 configs/protocol_profiles/。")
        title, description, kind, defaults = PRESETS.get(name, (name, "仓库配置", "stencil", {}))
        return {"id": name, "title": title, "description": description, "kind": kind,
                "defaults": defaults, "text": text, "profile_text": profile_path.read_text(),
                "profile_id": profile_path.name, "op": parsed.get("op", {})}

    def listing(self) -> dict[str, Any]:
        configs = [self.config(p.name) for p in sorted(self.config_dir.glob("*.cfg"))]
        order = list(PRESETS)
        configs.sort(key=lambda c: order.index(c["id"]) if c["id"] in order else 100)
        return {"configs": configs, "cli_fields": CLI_FIELDS,
                "profiles": [{"id": p.name, "text": p.read_text()} for p in sorted(self.profile_dir.glob("*.profile"))]}

    def prepare(self, request: RunRequest) -> dict[str, Any]:
        selected = self.config(request.config_id)
        text = request.config_text if request.config_text is not None else selected["text"]
        config = parse_values(text, sections=True)
        if not config.get("op") or not config.get("stack", {}).get("profile"):
            raise ValueError("配置需要 [stack] profile 和非空 [op]。")
        stack = config["stack"]
        profile_path = (self.repo/stack["profile"]).resolve()
        if request.profile_text is None:
            if profile_path.parent != self.profile_dir.resolve() or not profile_path.is_file():
                raise ValueError("请选择仓库 profile，或在编辑器内提供完整 profile 内容。")
            profile_text = profile_path.read_text()
        else:
            profile_text = request.profile_text
        profile = parse_values(profile_text)[""]
        if not profile:
            raise ValueError("profile 不能为空。")
        for key, value in request.stack_overrides.items():
            if key == "profile" or not re.fullmatch(r"[A-Za-z][\w]*", key):
                raise ValueError("profile 引用请在配置源码中修改。")
            stack[key] = scalar(value)
        resolved = {**profile, **{k: v for k, v in stack.items() if k != "profile"}}
        for key in ["bandwidthGbps", "interNodeBandwidthGbps", "numLanes", "linksPerGpu", "sprayChunkSize"]:
            if key in resolved and (not resolved[key].isdigit() or int(resolved[key]) < 1):
                raise ValueError(f"{key} 需要正整数。")
        for key in ["latencyNs", "interNodeLatencyNs"]:
            if key in resolved and (not resolved[key].isdigit() or int(resolved[key]) > 1000000000):
                raise ValueError(f"{key} 需要 0–1000000000 的整数。")
        if resolved.get("interNodeFabricType", "RoCE") not in {"ETH", "RoCE", "NVLink"}:
            raise ValueError("interNodeFabricType 需要 ETH、RoCE 或 NVLink。")
        if resolved.get("interNodeTopology", "fullmesh") not in CLI_FIELDS["interNodeTopology"]["choices"]:
            raise ValueError("interNodeTopology 不是支持的节点间拓扑。")
        cli = validate_cli(request.cli_overrides)
        if len(request.stack_overrides) > 80 or len(request.cli_overrides) > 80:
            raise ValueError("单次实验最多支持 80 项覆盖参数。")
        ranks = request.num_nodes * request.gpus_per_node if request.num_nodes > 1 else request.num_gpus
        for key in ["rootRank", "bgP2pSrc", "bgP2pDst"]:
            if key in cli and int(cli[key]) >= ranks:
                raise ValueError(f"{key} 必须小于总 rank 数 {ranks}。")
        if cli.get("randTraffic") in {"1", "true"} and request.num_nodes < 2:
            raise ValueError("随机跨节点流量需要至少两个节点。")
        op = config["op"]
        stencil = "collective" not in op
        if stencil and not any(key.startswith("transfer.") for key in op):
            raise ValueError("[op] 需要 collective / algorithm，或 transfer.* 传输。")
        notices = []
        if request.config_id == "h200-request-response.cfg" and stencil:
            notices.append("请求和响应大小由 transfer.*.bytes 决定；默认分别为 4 KiB 和 64 KiB。")
        if not stencil and any(k in stack for k in ["vcCredits", "vcCount", "flowControl"]):
            notices.append("collective 委托路径保留 inline VC 设置；这些 [stack] VC 键用于 stencil 路径。")
        if request.num_nodes > 1 and cli.get("interNodeTopology") and "interNodeTopology" in resolved:
            notices.append("[stack] interNodeTopology 会覆盖同名 CLI 选择；请以配置预览与结果为准。")
        summary = {"collective": "transfer stencil" if stencil else op.get("collective", ""),
                   "algorithm": "stencil" if stencil else op.get("algorithm", "auto"),
                   "topology": op.get("topology", "ring"), "ranks": ranks,
                   "num_nodes": request.num_nodes, "gpus_per_node": request.gpus_per_node,
                   "data_size": request.data_size, "stencil": stencil,
                   "inter_node_topology": resolved.get("interNodeTopology", cli.get("interNodeTopology", "fullmesh")),
                   "inter_node_bandwidth_gbps": resolved.get("interNodeBandwidthGbps", "400"),
                   "inter_node_latency_ns": resolved.get("interNodeLatencyNs", "10000"),
                   "fabric": resolved.get("interNodeFabricType", "RoCE") if request.num_nodes > 1 else "NVLink",
                   "bandwidth_gbps": resolved.get("bandwidthGbps", "800"),
                   "latency_ns": resolved.get("latencyNs", "500"),
                   "profile": Path(stack["profile"]).name,
                   "transfers": [{"src": v, "dst": op.get(k.replace(".src", ".dst"), "?"),
                                  "bytes": op.get(k.replace(".src", ".bytes"), "?")}
                                 for k, v in op.items() if k.startswith("transfer.") and k.endswith(".src")]}
        stack["profile"] = "profile.profile"
        rendered = "\n\n".join("["+section+"]\n"+"\n".join(f"{k} = {v}" for k, v in config[section].items())
                               for section in ["stack", "op"])+"\n"
        return {"config": rendered, "profile": profile_text, "resolved": resolved,
                "summary": summary, "notices": notices, "cli": cli,
                "title": request.name.strip() or selected["title"]}
