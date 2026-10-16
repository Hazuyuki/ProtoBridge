#!/usr/bin/env python3
"""
ns-3 tree-allreduce sweep + surrogate back-fit (ground truth = ns-3).

Ground truth: the chunk-pipelined full-volume ``TreeAllReduce`` (post
volume-fix; carries the full D over every tree edge in each direction) on
``--topology=switched`` with ``--linksPerGpu=18`` — the TRUE H200 fabric:
18 separate NVLink4 point-to-point links per GPU at the effective per-lane
rate (170 Gbps, the profile's ``bandwidthGbps``), with the endpoint's spray
routing round-robining 128 KiB chunks across all 18 links (aggregate trunk
= 18 x 170 = 3060 Gbps = 382500 B/us). The earlier single-link
``--linkDataRate=3060`` convention collapsed the 18 lanes into ONE p2p pipe
(no spray, no per-link queues) — retired. Sweeps
N in {4,8,16,32} x sizes 1 MB..256 MB, parses ``simTimeUs``, writes
``tree_sweep.csv``, then back-fits the ORIGINAL two-parameter form

    T = S(D) + steps * t_min_switch + alpha(N) * D / tree_bw

where steps = 2*floor(log2 N) and alpha(N) is the STRUCTURAL busiest
GPU<->NVSwitch trunk load in units of D (1 for N<=2, 2 for the root at
N<=4, 3 for an interior node at N>=5) -- only TWO constants are fitted:
(t_min_switch, tree_bw), by relative-error-weighted (1/T^2) least squares.

The D-dependent pipeline-fill residual (credit-window ramp-up + chunk
pipelines warming up: small-D ns-3 runs FASTER than the structural load,
large-D slightly slower) is deliberately NOT modeled -- it is absorbed into
the fitted pair. A dedicated additive fill term (commit a1fa05d) bought the
MAPE down to 3.96% but made the formula a 5-constant curve fit; that trade
was rejected in favor of the physical form.

FIT RESULT at this convention: t_min_switch = 4.10 us, tree_bw = 275400
B/us (eta = 0.72 vs the 382500 B/us trunk physics), MAPE 12.65% all-36
(per-N 10.6/11.0/14.0/15.1%).

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

LINKS_PER_GPU = 18             # true H200 fabric: 18 NVLink4 lanes per GPU
PER_LINK_GBPS = 170            # effective per-lane rate (profile bandwidthGbps)
TRUNK_PHYSICS = LINKS_PER_GPU * PER_LINK_GBPS * 1000 / 8   # 382500 B/us

# Profile startup delays (h200-ll128.profile), selected by total D.
STARTUP_US = {"LL": 15.0, "LL128": 25.0, "SIMPLE": 46.0}
SIMPLE_CHUNK_BYTES = 2 << 20  # chunk D/N >= 2 MB => SIMPLE wire regime

# Shipped surrogate constants (surrogate/theory/whitebox_surrogate_v2.py);
# the sweep re-derives them and asserts agreement.
SHIPPED = dict(t_min_switch_us=4.10,
               tree_bw_per_level_bytes_per_us=275400)


def alpha_of(n: int) -> float:
    """Structural alpha(N) -- the busiest-trunk load, same as the surrogate."""
    if n <= 2:
        return 1.0
    if n <= 4:
        return 2.0
    return 3.0


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
    arg = (f"gpu-cluster-sim --topology=switched --collective=allreduce "
           f"--algorithm=tree --numGpus={n} --dataSize={d} "
           f"--bandwidth={PER_LINK_GBPS} --latency=400 --numLanes=18 "
           f"--vcCredits=104857600 --fabricType=1 "
           f"--protocolModel=ns3::NcclProtocolModel "
           f"--linksPerGpu={LINKS_PER_GPU} --forceProtocol=0 "
           f"--startupLL=15000 --startupLL128=25000 --startupSIMPLE=46000 "
           f"--RngRun=1")
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


def fit_original_model(rows):
    """Joint relative-error-weighted (1/T^2) LS for (t_min_switch, tree_bw).

    Model: T = S(D) + steps*t_min_switch + alpha(N)*D/tree_bw with the
    STRUCTURAL alpha(N) = 1/2/3 (see alpha_of). Two unknowns, one 2x2
    normal-equation solve (columns: steps, alpha(N)*D; coefficients:
    t_min_switch, 1/tree_bw).
    Returns (mape, t_fixed, tree_bw).
    """
    A = [[float(steps_of(n)), alpha_of(n) * d] for n, d, _ in rows]
    y = [t - STARTUP_US[proto_of(d)] for _, d, t in rows]
    w = [1.0 / (t * t) for _, _, t in rows]
    coef = solve_linear(A, y, w)
    if coef is None:
        return None
    t_fixed, inv_bw = coef
    bw = 1.0 / inv_bw
    errs = []
    for (n, d, t), row in zip(rows, A):
        p = STARTUP_US[proto_of(d)] + sum(a * c for a, c in zip(row, coef))
        errs.append(abs(p - t) / t)
    m = 100.0 * sum(errs) / len(errs)
    return (m, t_fixed, bw)


def predict(n, d, t_fixed, tree_bw):
    s = steps_of(n)
    return (STARTUP_US[proto_of(d)] + s * t_fixed
            + alpha_of(n) * d / tree_bw)


def main(argv=None):
    import sys
    refit_only = bool(argv and len(argv) > 1 and argv[1] == "--refit")
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
    mape, t_fixed, tree_bw = fit_original_model(fit_rows)

    def mape_of(pts):
        errs = [abs(predict(n, d, t_fixed, tree_bw) - t) / t
                for (n, d, t) in pts]
        return 100.0 * sum(errs) / len(errs) if errs else float("nan")

    simple_pts = [(r["numGpus"], r["dataSize"], r["simTimeUs"])
                  for r in rows
                  if r["simTimeUs"] is not None
                  and r["wireRegime"] == "SIMPLE"]
    per_n = {n: mape_of([p for p in fit_rows if p[0] == n]) for n in NUM_GPUS}

    print(f"\n=== ORIGINAL 2-PARAM FIT ({len(fit_rows)} pts) ===")
    print(f"t_min_switch = {t_fixed:.3f} us")
    print(f"tree_bw      = {tree_bw:.0f} B/us "
          f"(eta = {tree_bw / TRUNK_PHYSICS:.4f} vs {TRUNK_PHYSICS:.0f} trunk)")
    print(f"MAPE (all {len(fit_rows)} pts) = {mape:.2f}%")
    print(f"MAPE (SIMPLE regime, {len(simple_pts)} pts) = "
          f"{mape_of(simple_pts):.2f}%")
    for n, m in per_n.items():
        print(f"  N={n:>2}: {m:.2f}%")

    json_path = Path(__file__).with_name("tree_calib.json")
    json_path.write_text(json.dumps(dict(
        source=("ns-3 TreeAllReduce (chunk-pipelined full-volume, post "
                "volume-fix + post local-rank-fix e1cdd9c) on "
                "--topology=switched; TRUE H200 fabric: --linksPerGpu=18 "
                "separate point-to-point NVLink4 lanes @ 170 Gbps each, "
                "spray-routed (128 KiB chunks round-robin), aggregate trunk "
                "3060 Gbps; credits unconstrained (--vcCredits=104857600)"),
        model=("T = S(D) + 2*log2(N)*t_min_switch + alpha(N)*D/tree_bw; "
               "alpha(N) = STRUCTURAL busiest GPU<->NVSwitch trunk load in D "
               "units (1/2/3); the pipeline-fill residual is absorbed into "
               "the fitted pair, not modeled"),
        fit_set=("all points, relative-error weighted (1/T^2); N in "
                 "{4,8,16,32} x 1-256 MB; TWO free constants "
                 "(t_min_switch, tree_bw), joint LS"),
        tree_bw_per_level_bytes_per_us=tree_bw,
        t_min_switch_us=t_fixed,
        alpha_structural={str(n): alpha_of(n) for n in NUM_GPUS},
        mape_all_pct=round(mape, 2),
        mape_simple_pct=round(mape_of(simple_pts), 2),
        mape_per_n_pct={str(n): round(m, 2) for n, m in per_n.items()},
        n_fit_points=len(fit_rows),
        ns3_config=dict(topology="switched", links_per_gpu=LINKS_PER_GPU,
                        per_link_gbps=PER_LINK_GBPS,
                        trunk_physics_gbps=LINKS_PER_GPU * PER_LINK_GBPS,
                        startup_us=STARTUP_US),
        trunk_physics_bytes_per_us=TRUNK_PHYSICS,
        sweep_csv=csv_path.name,
        shipped_constants=SHIPPED,
    ), indent=2))
    print(f"Wrote {json_path}")


if __name__ == "__main__":
    import sys
    main(sys.argv)
