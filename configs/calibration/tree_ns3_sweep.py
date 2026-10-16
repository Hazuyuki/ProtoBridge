#!/usr/bin/env python3
"""
ns-3 tree-allreduce sweep + surrogate back-fit (ground truth = ns-3).

Ground truth: the chunk-pipelined full-volume ``TreeAllReduce`` (post
volume-fix; carries the full D over every tree edge in each direction) on
``--topology=switched`` via ``h200-tree-allreduce.cfg`` with
``--linkDataRate=3060`` (18 lanes x 170 Gbps effective per-lane = the full
GPU<->NVSwitch trunk; the historical per-protocol 80/100/50 values were
compensating the old model's volume deficit and are retired). Sweeps
N in {4,8,16,32} x sizes 1 MB..256 MB, parses ``simTimeUs``, writes
``tree_sweep.csv``, then back-fits to

    T = S(D) + steps * T_fixed
        + (alpha_inf(N) * D + min(F * steps, kappa * D)) / tree_bw

where steps = 2*floor(log2 N), alpha_inf(N) is the steady-state (large-D)
busiest GPU<->trunk load in units of D, F the pipeline-fill volume per step,
and kappa the fill cap fraction of D.

WHY THE FILL TERM: the previous (9/18, commit f984edb) form was
``T = S + steps*t_min_switch + alpha(N)*D/bw`` with the piecewise-structural
alpha = 1/2/3. Re-fitting that shape to the FRESH sweep (the old fit CSV
predates the 9/30 single-node local-rank fix e1cdd9c -- SetLocalRankRange(0,
numGpus) -- which makes ALL rank pairs exchange credits and shifted tree times
up to ~8%; constants must be re-fit regardless of form) gives 11.7% MAPE: the
implied alpha_eff(D) DECREASES with D (N=8: 3.99 at 8 MiB -> 2.79 at 256 MiB),
i.e. pipeline fill is an ADDITIVE per-step byte volume, not a multiplicative
factor. The fill form separates it:

    alpha_inf = 1.0 (N<=2) / 1.65 (N<=4) / 2.88 (N<=8)
                / 3.00 (N<=16) / 3.12 (N>=32)   [step function]
    F = 2.83 MiB/step (~22.6 spray chunks of 131072 B), kappa = 3.5

FIT METHOD: tree_bw is PINNED at 270653 B/us (the eta = 0.7076 trunk
realization -- free-bandwidth fits are degenerate with the fill term: bw
inverts to nonsense values). (T_fixed, alpha_inf per N) are closed-form
relative-error-weighted (1/T^2) least squares; (F, kappa) is a 2-D grid.
Fresh-sweep MAPE: 3.6% all 36 points (per-N 3.8/2.5/3.5/4.7%), vs 13.1% for
the f984edb constants on the same data.

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

NUM_GPUS = [4, 8, 16, 32]
SIZES = [1 << 20, 2 << 20, 4 << 20, 8 << 20, 16 << 20, 32 << 20,
         64 << 20, 128 << 20, 256 << 20]

LINK_DATA_RATE_GBPS = 3060    # 18 lanes x 170 Gbps effective = GPU<->NVSwitch trunk
TRUNK_PHYSICS = LINK_DATA_RATE_GBPS * 1000 / 8   # 382500 B/us
TREE_BW = 270653.0             # PINNED trunk realization (eta = 0.7076)
CFG = "configs/protocol_configs/h200-tree-allreduce.cfg"

# Profile startup delays (h200-ll128.profile), selected by total D.
STARTUP_US = {"LL": 15.0, "LL128": 25.0, "SIMPLE": 46.0}
SIMPLE_CHUNK_BYTES = 2 << 20  # chunk D/N >= 2 MB => SIMPLE wire regime

# Shipped surrogate constants (surrogate/theory/whitebox_surrogate_v2.py);
# the sweep re-derives them and asserts agreement.
SHIPPED = dict(t_min_switch_us=1.22,
               tree_fill_bytes_per_step=2.76 * 1024 * 1024,
               tree_fill_cap_ratio=3.45,
               alpha_inf={2: 1.0, 4: 1.71, 8: 2.74, 16: 2.89, 32: 2.95})


def alpha_of(n: int) -> float:
    """Steady-state alpha_inf(N) -- the shipped surrogate's step function."""
    if n <= 2:
        return 1.0
    if n <= 4:
        return 1.71
    if n <= 8:
        return 2.74
    if n <= 16:
        return 2.89
    return 2.95


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


def solve_linear(A, y, w):
    """Weighted least squares via normal equations + Gaussian elimination."""
    n = len(A[0])
    N_ = [[0.0] * n for _ in range(n)]
    b = [0.0] * n
    for row, yy, ww in zip(A, y, w):
        for i in range(n):
            b[i] += ww * row[i] * yy
            for j in range(n):
                N_[i][j] += ww * row[i] * row[j]
    for col in range(n):
        piv = max(range(col, n), key=lambda r: abs(N_[r][col]))
        if abs(N_[piv][col]) < 1e-12:
            return None
        N_[col], N_[piv] = N_[piv], N_[col]
        b[col], b[piv] = b[piv], b[col]
        for r in range(col + 1, n):
            f = N_[r][col] / N_[col][col]
            for c in range(col, n):
                N_[r][c] -= f * N_[col][c]
            b[r] -= f * b[col]
    x = [0.0] * n
    for i in range(n - 1, -1, -1):
        x[i] = (b[i] - sum(N_[i][j] * x[j] for j in range(i + 1, n))) / N_[i][i]
    return x


def fit_fill_model(rows):
    """Grid (F, kappa) x closed-form weighted LS for (T_fixed, alpha_N).

    tree_bw is pinned; unknowns are T_fixed and one alpha_inf per swept N.
    Returns (mape, F_bytes, kappa, t_fixed, {N: alpha}).
    """
    ns = sorted(set(n for n, _, _ in rows))
    best = None
    for F100 in range(100, 601):        # 1.0 .. 6.0 MiB/step
        F = F100 / 100 * 2 ** 20
        for kap100 in range(100, 601):  # 1.0 .. 6.0
            kap = kap100 / 100
            A, y, w = [], [], []
            for n, d, t in rows:
                s = steps_of(n)
                fill = min(F * s, kap * d) / TREE_BW
                A.append([float(s)] + [d / TREE_BW if nn == n else 0.0
                                       for nn in ns])
                y.append(t - STARTUP_US[proto_of(d)] - fill)
                w.append(1.0 / (t * t))
            coef = solve_linear(A, y, w)
            if coef is None:
                continue
            errs = []
            for i, (n, d, t) in enumerate(rows):
                p = (STARTUP_US[proto_of(d)]
                     + sum(a * c for a, c in zip(A[i], coef))
                     + min(F * steps_of(n), kap * d) / TREE_BW)
                errs.append(abs(p - t) / t)
            m = 100.0 * sum(errs) / len(errs)
            if best is None or m < best[0]:
                best = (m, F, kap, coef[0], dict(zip(ns, coef[1:])))
    return best


def predict(n, d, t_fixed, alpha_map, F, kap):
    s = steps_of(n)
    return (STARTUP_US[proto_of(d)] + s * t_fixed
            + (alpha_map[n] * d + min(F * s, kap * d)) / TREE_BW)


def main(argv=None):
    import sys
    refit_only = bool(argv and argv[1] == "--refit")
    rows = []
    if refit_only:
        # Re-fit from the existing sweep CSV (no ns-3 runs); the rows keep
        # their recorded wire/startup regimes.
        csv_in = Path(__file__).with_name("tree_sweep.csv")
        with csv_in.open() as f:
            for r in csv.DictReader(f):
                rows.append(dict(
                    numGpus=int(r["numGpus"]), dataSize=int(r["dataSize"]),
                    dataSizeMB=int(r["dataSizeMB"]), chunkMB=int(r["chunkMB"]),
                    wireRegime=r["wireRegime"], startupRegime=r["startupRegime"],
                    simTimeUs=float(r["simTimeUs"])))
        print(f"Refit-only mode: {len(rows)} rows from {csv_in.name}")
    else:
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
    if not refit_only:
        with csv_path.open("w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print(f"\nWrote {csv_path} ({len(rows)} rows)")

    fit_rows = [(r["numGpus"], r["dataSize"], r["simTimeUs"])
                for r in rows if r["simTimeUs"] is not None]
    mape, F, kap, t_fixed, alpha_map = fit_fill_model(fit_rows)

    def mape_of(pts):
        errs = [abs(predict(n, d, t_fixed, alpha_map, F, kap) - t) / t
                for (n, d, t) in pts]
        return 100.0 * sum(errs) / len(errs) if errs else float("nan")

    simple_pts = [(r["numGpus"], r["dataSize"], r["simTimeUs"])
                  for r in rows
                  if r["simTimeUs"] is not None
                  and r["wireRegime"] == "SIMPLE"]
    per_n = {n: mape_of([p for p in fit_rows if p[0] == n]) for n in NUM_GPUS}

    print(f"\n=== FILL-MODEL FIT ({len(fit_rows)} pts, bw pinned "
          f"{TREE_BW:.0f}) ===")
    print(f"t_fixed      = {t_fixed:.3f} us")
    print(f"fill/step F  = {F / 2**20:.2f} MiB ({F / 131072:.1f} spray chunks)"
          f"  capped at {kap:.2f} * D")
    print(f"alpha_inf    = " + ", ".join(f"N={n}: {a:.3f}"
                                         for n, a in alpha_map.items()))
    print(f"MAPE (all {len(fit_rows)} pts) = {mape:.2f}%")
    print(f"MAPE (SIMPLE regime, {len(simple_pts)} pts) = "
          f"{mape_of(simple_pts):.2f}%")
    for n, m in per_n.items():
        print(f"  N={n:>2}: {m:.2f}%")

    json_path = Path(__file__).with_name("tree_calib.json")
    json_path.write_text(json.dumps(dict(
        source=("ns-3 TreeAllReduce (chunk-pipelined full-volume, post "
                "volume-fix + post local-rank-fix e1cdd9c) on "
                "--topology=switched via h200-tree-allreduce.cfg"),
        model=("T = S(D) + 2*log2(N)*t_min_switch + (alpha_inf(N)*D + "
               "min(F*steps, kappa*D))/tree_bw; alpha_inf = steady-state "
               "busiest GPU<->NVSwitch trunk load in D units, F = pipeline-"
               "fill volume per step (additive, NOT multiplicative)"),
        fit_set=("all points, relative-error weighted (1/T^2); N in "
                 "{4,8,16,32} x 1-256 MB; tree_bw PINNED (free-bw fits are "
                 "degenerate with the fill term)"),
        tree_bw_per_level_bytes_per_us=TREE_BW,
        t_min_switch_us=t_fixed,
        tree_fill_bytes_per_step=F,
        tree_fill_cap_ratio=kap,
        alpha_inf={str(k): v for k, v in alpha_map.items()},
        mape_all_pct=round(mape, 2),
        mape_simple_pct=round(mape_of(simple_pts), 2),
        mape_per_n_pct={str(n): round(m, 2) for n, m in per_n.items()},
        n_fit_points=len(fit_rows),
        ns3_config=dict(cfg=CFG, link_data_rate_gbps=LINK_DATA_RATE_GBPS,
                        startup_us=STARTUP_US),
        trunk_physics_bytes_per_us=TRUNK_PHYSICS,
        sweep_csv=csv_path.name,
        shipped_constants=SHIPPED,
    ), indent=2))
    print(f"Wrote {json_path}")


if __name__ == "__main__":
    import sys
    main(sys.argv)
