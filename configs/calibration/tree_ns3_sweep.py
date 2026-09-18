#!/usr/bin/env python3
"""
ns-3 tree-allreduce sweep + surrogate back-fit (ground truth = ns-3).

Ground truth: the chunk-pipelined full-volume ``TreeAllReduce`` (post
volume-fix; carries the full D over every tree edge in each direction) on
``--topology=switched`` via ``h200-tree-allreduce.cfg`` with
``--linkDataRate=3060`` (18 lanes x 170 Gbps effective per-lane = the full
GPU<->NVSwitch trunk; the historical per-protocol 80/100/50 values were
compensating the old model's volume deficit and are retired). Sweeps
N in {4,8,16} x sizes 1 MB..256 MB, parses ``simTimeUs``, writes
``tree_sweep.csv``, then back-fits (``tree_bw_per_level``, ``t_min_switch``)
over ALL points (relative-error weighted least squares, weight 1/T^2, so
small and large sizes count equally) to

    T = S(D) + steps * T_fixed + alpha(N) * D / tree_bw

where steps = 2*floor(log2 N) and alpha(N) is the busiest GPU<->trunk load
in units of D for the heap-shaped binary tree with overlapped reduce-up +
broadcast-down chunk pipelines:

    alpha(N) = 1 (N<=2: single edge), 2 (N in 3..4: root receives/sends 2D),
               3 (N>=5: an interior node moves 2D child + 1D parent traffic)

(A SIMPLE-regime-only unweighted fit — the full-mesh precedent — lands at
bw=294090/t_fixed=9.45 with SIMPLE MAPE 2.1% but a 9.45 us intercept that is
unphysical and inflates <=4 MB points by up to ~90%; the weighted all-point
fit trades that for a balanced 7.6%/5.0% all/SIMPLE MAPE with a physical
~3 us switch-pipeline intercept.) Writes ``tree_calib.json``.

Run:  python3 configs/calibration/tree_ns3_sweep.py
"""

from __future__ import annotations

import csv
import json
import math
import os
import signal
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

NUM_GPUS = [4, 8, 16]
SIZES = [1 << 20, 2 << 20, 4 << 20, 8 << 20, 16 << 20, 32 << 20,
         64 << 20, 128 << 20, 256 << 20]

LINK_DATA_RATE_GBPS = 3060    # 18 lanes x 170 Gbps effective = GPU<->NVSwitch trunk
TRUNK_PHYSICS = LINK_DATA_RATE_GBPS * 1000 / 8   # 382500 B/us
CFG = "configs/protocol_configs/h200-tree-allreduce.cfg"

# Profile startup delays (h200-ll128.profile), selected by total D.
STARTUP_US = {"LL": 15.0, "LL128": 25.0, "SIMPLE": 46.0}
SIMPLE_CHUNK_BYTES = 2 << 20  # chunk D/N >= 2 MB => SIMPLE wire regime


def alpha_of(n: int) -> int:
    if n <= 2:
        return 1
    if n <= 4:
        return 2
    return 3


def steps_of(n: int) -> int:
    return 2 * max(1, int(math.log2(n)))


def proto_of(size: int) -> str:
    if size < 8192:
        return "LL"
    if size < (2 << 20):
        return "LL128"
    return "SIMPLE"


def run_ns3(n, d, timeout=600):
    """Run one ns-3 tree point; return simTimeUs or None.

    start_new_session + os.killpg on timeout prevents orphan ns-3 processes
    (per the batch-runner killpg fix).
    """
    arg = (f"gpu-cluster-sim --protocolConfig={CFG} --numGpus={n} "
           f"--dataSize={d} --linkDataRate={LINK_DATA_RATE_GBPS} --RngRun=1")
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


def main():
    rows = []
    for n in NUM_GPUS:
        for d in SIZES:
            t = run_ns3(n, d)
            chunk = d // n
            rows.append(dict(
                numGpus=n, dataSize=d, dataSizeMB=d >> 20,
                chunkMB=chunk >> 20, wireRegime=proto_of(chunk),
                startupRegime=proto_of(d), simTimeUs=t))
            print(f"N={n:2d} D={d >> 20:4d}MB chunk={chunk >> 20:3d}MB "
                  f"({proto_of(chunk):6s}) -> "
                  f"{'FAILED' if t is None else f'{t:.2f} us'}")

    csv_path = Path(__file__).with_name("tree_sweep.csv")
    with csv_path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"\nWrote {csv_path} ({len(rows)} rows)")

    # ---- Back-fit (T_fixed, 1/tree_bw), relative-error weighted, all points ----
    # Model residual: T - S = steps * T_fixed + (alpha*D) * inv_bw.
    # Weighted (1/T^2) 2x2 normal equations for x = (T_fixed, inv_bw).
    a11 = a12 = a22 = b1 = b2 = 0.0
    fit_pts = []
    for r in rows:
        if r["simTimeUs"] is None:
            continue
        n, d, t = r["numGpus"], r["dataSize"], r["simTimeUs"]
        s = steps_of(n)
        v = alpha_of(n) * d
        resid = t - STARTUP_US[r["startupRegime"]]
        w = 1.0 / (t * t)
        a11 += w * s * s
        a12 += w * s * v
        a22 += w * v * v
        b1 += w * s * resid
        b2 += w * v * resid
        fit_pts.append((n, d, t))
    det = a11 * a22 - a12 * a12
    t_fixed = (b1 * a22 - b2 * a12) / det
    inv_bw = (a11 * b2 - a12 * b1) / det
    tree_bw = 1.0 / inv_bw

    def predict(n, d):
        return (STARTUP_US[proto_of(d)] + steps_of(n) * t_fixed
                + alpha_of(n) * d / tree_bw)

    def mape(pts):
        errs = [abs(predict(n, d) - t) / t for (n, d, t) in pts]
        return 100.0 * sum(errs) / len(errs) if errs else float("nan")

    all_pts = [(r["numGpus"], r["dataSize"], r["simTimeUs"])
               for r in rows if r["simTimeUs"] is not None]
    simple_pts = [(r["numGpus"], r["dataSize"], r["simTimeUs"])
                  for r in rows
                  if r["simTimeUs"] is not None and r["wireRegime"] == "SIMPLE"]
    eta = tree_bw / TRUNK_PHYSICS

    print(f"\n=== FIT (relative-error weighted, {len(fit_pts)} pts) ===")
    print(f"tree_bw (trunk) = {tree_bw:.1f} B/us  (eta_tree = {eta:.4f} vs "
          f"trunk physics {TRUNK_PHYSICS:.0f})")
    print(f"t_min_switch    = {t_fixed:.3f} us")
    print(f"MAPE (all {len(all_pts)} pts) = {mape(all_pts):.2f}%")
    print(f"MAPE (SIMPLE regime, {len(simple_pts)} pts) = {mape(simple_pts):.2f}%")

    json_path = Path(__file__).with_name("tree_calib.json")
    json_path.write_text(json.dumps(dict(
        source=("ns-3 TreeAllReduce (chunk-pipelined full-volume, post "
                "volume-fix) on --topology=switched via h200-tree-allreduce.cfg"),
        model=("T = S(D) + 2*log2(N)*t_min_switch + alpha(N)*D/tree_bw; "
               "alpha = 1 (N<=2) / 2 (N<=4) / 3 (N>=5) = busiest "
               "GPU<->NVSwitch trunk load in D units"),
        fit_set=("all points, relative-error weighted (1/T^2); N in {4,8,16}"
                 " x 1-256 MB"),
        tree_bw_per_level_bytes_per_us=tree_bw,
        t_min_switch_us=t_fixed,
        eta_tree=eta,
        mape_all_pct=mape(all_pts),
        mape_simple_pct=mape(simple_pts),
        n_fit_points=len(fit_pts),
        ns3_config=dict(cfg=CFG, link_data_rate_gbps=LINK_DATA_RATE_GBPS,
                        startup_us=STARTUP_US),
        trunk_physics_bytes_per_us=TRUNK_PHYSICS,
        sweep_csv=csv_path.name,
    ), indent=2))
    print(f"Wrote {json_path}")


if __name__ == "__main__":
    main()
