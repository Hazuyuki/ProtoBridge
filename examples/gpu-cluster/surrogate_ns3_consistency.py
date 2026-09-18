#!/usr/bin/env python3
"""
Surrogate 4-preset vs ns-3 consistency regression (report, not a gate).

Runs the current ns-3 injectors for all four surrogate algorithm presets
(ring / tree / nvls / fullmesh) at N=8 and compares against the
theory-derived surrogate, recording per-point ratios and per-preset MAPE in
``surrogate_ns3_consistency.csv`` / ``.json``. Each preset has a DIFFERENT
documented anchoring, so the expected bands differ (see ``notes`` in the
JSON):

  ring     HW back-fit (ring_bw=177000 anchored to the measured H200 8-GPU
           point) -- a lower bound vs ns-3, not an ns-3 fit.
  tree     ns-3 back-fit (this session: tree_bw=270653, t_min_switch=2.99,
           alpha(N) trunk model; fit MAPE 7.6% all / 5.0% SIMPLE).
  nvls     HW-multicast-derived (nvls_bw=535500 = 1.2 x B_agg), recorded
           against TWO ns-3 conventions (single-170G-link cfg default and
           the --linkDataRate=3060 full trunk) because they differ ~6x; the
           historical "~0.57x lower-bound" note matches neither and its
           conditions are unrecorded (drift finding, 2026-09). Sizes
           restricted to 4-16 MiB (>=64 MiB exceeds the sim-time budget,
           see the NVLS scaling notes).
  fullmesh ns-3 back-fit (fullmesh_bw=449890, MAPE 0.10% SIMPLE regime).

Run:  python3 examples/gpu-cluster/surrogate_ns3_consistency.py
"""

from __future__ import annotations

import csv
import json
import math
import os
import signal
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "surrogate" / "theory"))
import whitebox_surrogate_v2 as wb  # noqa: E402

MB = 1 << 20

# h200-ll128.profile startup tiers, selected by total D (ns-3 GetProtocolId).
STARTUP_US = {"LL": 15.0, "LL128": 25.0, "SIMPLE": 46.0}


def proto_of(size: int) -> str:
    if size < 8192:
        return "LL"
    if size < (2 << 20):
        return "LL128"
    return "SIMPLE"


def run_ns3(arg: str, timeout: int = 900):
    """Run one ns-3 point; return simTimeUs or None (killpg on timeout)."""
    try:
        proc = subprocess.Popen(
            ["./ns3", "run", arg], cwd=REPO,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, start_new_session=True)
    except Exception:
        return None
    try:
        out, _ = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        proc.communicate()
        return None
    for line in out.splitlines():
        if line.strip().startswith("simTimeUs"):
            try:
                return float(line.split("=", 1)[1].strip())
            except (ValueError, IndexError):
                return None
    return None


# ---- Per-preset point definitions ----------------------------------------

N = 8
RING_SIZES = [4 * MB, 16 * MB, 64 * MB, 256 * MB]
TREE_SIZES = [4 * MB, 16 * MB, 64 * MB, 256 * MB]
NVLS_SIZES = [4 * MB, 8 * MB, 16 * MB]          # >=64 MiB: sim-time budget
FULLMESH_SIZES = [16 * MB, 64 * MB, 256 * MB]   # SIMPLE regime (chunk >= 2 MB)


def points():
    """Yield (preset, D, ns3_arg, surrogate_us, note)."""
    ring_s = wb.make_h200_ring_theory()
    for d in RING_SIZES:
        arg = ("gpu-cluster-sim --protocolConfig="
               "configs/protocol_configs/h200-ring-allreduce.cfg "
               f"--numGpus={N} --dataSize={d} --RngRun=1")
        pred = ring_s.predict(d, N, "ring", credits=128, ber=0,
                              startup_us=STARTUP_US[proto_of(d)],
                              collective="allreduce")
        yield "ring", d, arg, pred, "HW back-fit; lower bound vs ns-3"

    tree_s = wb.make_h200_tree_theory()
    for d in TREE_SIZES:
        arg = ("gpu-cluster-sim --protocolConfig="
               "configs/protocol_configs/h200-tree-allreduce.cfg "
               f"--numGpus={N} --dataSize={d} --linkDataRate=3060 --RngRun=1")
        pred = tree_s.predict(d, N, "tree", credits=128, ber=0,
                              startup_us=STARTUP_US[proto_of(d)],
                              collective="allreduce")
        yield "tree", d, arg, pred, "ns-3 back-fit (this recalibration)"

    nvls_s = wb.make_h200_nvls_theory()
    for d in NVLS_SIZES:
        # Preset convention: nvls_startup_us=23. Two ns-3 conventions are
        # recorded because they differ 6x and the historical "~0.57x
        # lower-bound" ratio (conditions unrecorded) matches neither:
        #  - cfg default: each GPU hangs off the NVSwitch by ONE link at
        #    --bandwidth=170 Gbps (21250 B/us) -> ns-3 serializes the whole
        #    allgather egress on it; the preset's 535500 B/us multicast
        #    aggregate sits ~0.09-0.16x below.
        #  - --linkDataRate=3060 (the full 18x170 trunk, the same switched-
        #    fabric convention the tree calibration anchors): the preset
        #    lands ~0.9-1.2x -- NOT a lower bound in this convention (the
        #    preset's eta_nvls=1.2 multicast amplification exceeds the
        #    382500 B/us trunk).
        pred = nvls_s.predict(d, N, "nvls", credits=128, ber=0,
                              collective="allgather")
        arg = ("gpu-cluster-sim --protocolConfig="
               "configs/protocol_configs/h200-nvls-allgather.cfg "
               f"--numGpus={N} --dataSize={d} --RngRun=1")
        yield "nvls", d, arg, pred, "preset vs single-170G-link switched cfg"
        arg_trunk = arg.replace("--RngRun=1",
                                "--linkDataRate=3060 --RngRun=1")
        yield "nvls_trunk3060", d, arg_trunk, pred, \
            "preset vs full-trunk switched cfg (tree-calibration convention)"

    fm_s = wb.make_h200_fullmesh_theory()
    for d in FULLMESH_SIZES:
        arg = (f"gpu-cluster-sim --numGpus={N} --topology=fullmesh "
               f"--algorithm=fullmesh --collective=allreduce --dataSize={d} "
               "--bandwidth=200 --latency=400 --numLanes=18 "
               "--startupSIMPLE=15000 --RngRun=1")
        pred = fm_s.predict(d, N, "fullmesh", credits=128, ber=0,
                            collective="allreduce")
        yield "fullmesh", d, arg, pred, "ns-3 back-fit (MAPE 0.10%)"


def main():
    rows = []
    for preset, d, arg, pred, note in points():
        t = run_ns3(arg)
        ratio = (pred / t) if t else float("nan")
        err = abs(pred - t) / t * 100 if t else float("nan")
        rows.append(dict(preset=preset, numGpus=N, dataSizeMB=d >> 20,
                         ns3SimTimeUs=t, surrogateUs=round(pred, 2),
                         ratio=round(ratio, 4) if t else None,
                         absErrPct=round(err, 2) if t else None, note=note))
        print(f"{preset:8s} D={d >> 20:4d}MB ns3="
              f"{'FAIL' if t is None else f'{t:9.2f}'} "
              f"sur={pred:9.2f} ratio="
              f"{'-' if t is None else f'{ratio:.3f}'}")

    csv_path = Path(__file__).with_name("surrogate_ns3_consistency.csv")
    with csv_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    summary = {}
    for preset in ("ring", "tree", "nvls", "nvls_trunk3060", "fullmesh"):
        pts = [r for r in rows if r["preset"] == preset
               and r["ns3SimTimeUs"] is not None]
        if not pts:
            summary[preset] = dict(points=0)
            continue
        errs = [r["absErrPct"] for r in pts]
        ratios = [r["ratio"] for r in pts]
        summary[preset] = dict(
            points=len(pts),
            mape_pct=round(sum(errs) / len(errs), 2),
            ratio_min=min(ratios), ratio_max=max(ratios),
            note=pts[0]["note"])

    json_path = Path(__file__).with_name("surrogate_ns3_consistency.json")
    json_path.write_text(json.dumps(dict(
        purpose=("4-preset surrogate vs current ns-3 consistency regression; "
                 "report, not a pass/fail gate -- each preset's expected band "
                 "follows its anchoring (see notes)"),
        num_gpus=N,
        summary=summary,
        rows=rows,
    ), indent=2))
    print(f"\nWrote {csv_path}\nWrote {json_path}")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
