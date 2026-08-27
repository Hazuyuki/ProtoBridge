#!/usr/bin/env python3
"""
ns-3 full-mesh collective sweep + surrogate back-fit (ground truth = ns-3).

Ground truth: ns-3 ``FullMeshAllReduce`` / ``FullMeshAllGather`` on
``--topology=fullmesh`` (every GPU-pair gets a dedicated ``--numLanes=18``
bundle, no switch). Sweeps N in {4,8,16} x {allreduce,allgather} x sizes
1 MB..256 MB, parses ``simTimeUs``, writes ``fullmesh_sweep.csv``, then
back-fits the single surrogate scalar ``fullmesh_bw`` to

    T = S + steps * (lat + (D/N) / fullmesh_bw)

on the SIMPLE-regime points (chunk D/N >= 2 MB => wire-eff 1.0,
``startupSIMPLE=15000`` ns == surrogate ``startup_us=15.0``). Writes
``fullmesh_calib.json``.

Model (per the ns-3 fullmesh injector, fullmesh-allreduce.cc): each phase
ships the D/N chunk to all N-1 peers CONCURRENTLY on dedicated bundles, so a
phase's time = (D/N)/bundle_bw -- NO N-fold serialization (unlike nvls, which
funnels N chunks through one shared switch port). AR = 2 phases
(reduce-scatter + all-gather, barrier-gated), AG = 1 phase. Least-squares
scalar fit: fullmesh_bw = sum(V^2)/sum(R*V), V = steps*(D/N),
R = T_ns3 - S - steps*lat.

Run:  python3 configs/calibration/fullmesh_ns3_sweep.py
"""

from __future__ import annotations

import csv
import json
import os
import signal
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

NUM_GPUS = [4, 8, 16]
COLLECTIVES = ["allreduce", "allgather"]
SIZES = [1 << 20, 2 << 20, 4 << 20, 8 << 20, 16 << 20, 32 << 20,
         64 << 20, 128 << 20, 256 << 20]

BANDWIDTH_GBPS = 200          # 200 Gbps/link = 25 GB/s NVLink4 (B_agg=450000)
LATENCY_NS = 400
NUM_LANES = 18
STARTUP_SIMPLE_NS = 15000    # == surrogate startup_us=15.0
SIMPLE_CHUNK_BYTES = 2 << 20  # chunk D/N >= 2 MB => SIMPLE wire regime

# Surrogate constants the fit anchors to (must match make_h200_fullmesh_theory).
S_STARTUP_US = 15.0
LINK_LAT_US = 0.4
PHYSICS_BUNDLE = 25000 * 18   # per-link 25 GB/s * 18 lanes = 450000 B/us


def run_ns3(n, collective, d, timeout=300):
    """Run one ns-3 fullmesh point; return simTimeUs or None.

    start_new_session + os.killpg on timeout prevents orphan ns-3 processes
    (per the batch-runner killpg fix).
    """
    arg = (f"gpu-cluster-sim --numGpus={n} --topology=fullmesh "
           f"--algorithm=fullmesh --collective={collective} --dataSize={d} "
           f"--bandwidth={BANDWIDTH_GBPS} --latency={LATENCY_NS} "
           f"--numLanes={NUM_LANES} --startupSIMPLE={STARTUP_SIMPLE_NS} "
           f"--RngRun=1")
    try:
        proc = subprocess.Popen(
            ["./ns3", "run", arg], cwd=REPO,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
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


def regime_of(chunk):
    if chunk >= SIMPLE_CHUNK_BYTES:
        return "SIMPLE"
    if chunk >= 8192:
        return "LL128"
    return "LL"


def main():
    rows = []
    for n in NUM_GPUS:
        for coll in COLLECTIVES:
            for d in SIZES:
                t = run_ns3(n, coll, d)
                chunk = d // n
                rows.append(dict(
                    numGpus=n, collective=coll, dataSize=d, dataSizeMB=d >> 20,
                    chunkMB=chunk >> 20, regime=regime_of(chunk), simTimeUs=t))
                print(f"N={n:2d} {coll:9s} D={d >> 20:4d}MB "
                      f"chunk={chunk >> 20:3d}MB ({regime_of(chunk):5s}) -> "
                      f"{'FAILED' if t is None else f'{t:.2f} us'}")

    csv_path = Path(__file__).with_name("fullmesh_sweep.csv")
    with csv_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\nWrote {csv_path} ({len(rows)} rows)")

    # ---- Back-fit fullmesh_bw on SIMPLE-regime points ----
    sv = 0.0
    rv = 0.0
    fit_pts = []
    for r in rows:
        if r["simTimeUs"] is None or r["regime"] != "SIMPLE":
            continue
        steps = 2 if r["collective"] == "allreduce" else 1
        v = steps * (r["dataSize"] / r["numGpus"])
        resid = r["simTimeUs"] - S_STARTUP_US - steps * LINK_LAT_US
        sv += v * v
        rv += resid * v
        fit_pts.append((r["numGpus"], r["collective"], r["dataSize"],
                        r["simTimeUs"], steps))
    fullmesh_bw = sv / rv if rv else float("nan")

    errs = []
    for (n, coll, d, t, steps) in fit_pts:
        pred = S_STARTUP_US + steps * (LINK_LAT_US + (d / n) / fullmesh_bw)
        errs.append(abs(pred - t) / t)
    mape = 100.0 * sum(errs) / len(errs) if errs else float("nan")
    eta = fullmesh_bw / PHYSICS_BUNDLE

    print(f"\n=== FIT (SIMPLE regime, {len(fit_pts)} pts) ===")
    print(f"fullmesh_bw = {fullmesh_bw:.1f} B/us  (eta_fullmesh = {eta:.4f}, "
          f"vs physics {PHYSICS_BUNDLE} = {fullmesh_bw / PHYSICS_BUNDLE * 100:.2f}%)")
    print(f"MAPE = {mape:.2f}%")

    json_path = Path(__file__).with_name("fullmesh_calib.json")
    json_path.write_text(json.dumps(dict(
        source="ns-3 FullMeshAllReduce/FullMeshAllGather on --topology=fullmesh",
        model="T = startup + steps*(lat + (D/N)/fullmesh_bw); AR steps=2, AG steps=1",
        fit_set="SIMPLE regime (chunk D/N >= 2 MB, startupSIMPLE=15000 ns == surrogate startup_us=15.0)",
        fullmesh_bw_bytes_per_us=fullmesh_bw,
        eta_fullmesh=eta,
        mape_pct=mape,
        n_fit_points=len(fit_pts),
        ns3_config=dict(bandwidth_gbps=BANDWIDTH_GBPS, latency_ns=LATENCY_NS,
                        num_lanes=NUM_LANES, startup_simple_ns=STARTUP_SIMPLE_NS),
        surrogate_anchor=dict(startup_us=S_STARTUP_US, link_latency_us=LINK_LAT_US),
        physics_bundle_bytes_per_us=PHYSICS_BUNDLE,
        sweep_csv=csv_path.name,
    ), indent=2))
    print(f"Wrote {json_path}")


if __name__ == "__main__":
    main()
