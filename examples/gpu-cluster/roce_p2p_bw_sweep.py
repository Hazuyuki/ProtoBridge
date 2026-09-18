#!/usr/bin/env python3
"""
ns-3 RoCE v2 P2P sustained-throughput validation vs the ib_write_bw per-QP ceiling.

A single P2P transfer (one flowKey == one QP) of a LARGE dataSize pipelines MTU
chunks back-to-back on that one flow; sustained throughput = bytes / simTimeUs.
Without a per-QP cap, ns-3 lets a single QP hit the link line rate (~350 Gbps at
400G) -- WRONG vs the blog: a single QP sustains only ~49 Gbps (the ConnectX-7
per-QP ib_write_bw ceiling, configs/calibration/roce_p2p_calib.json
per_qp_ceiling_gbps = 49.0, bandwidth_fit.R_gbps = 49.26). The opt-in
--perQpCeilingGbps mounts DcqcnRateMachine's true per-flow pacer, which caps the
single-QP sustained rate at the ceiling.

This script sweeps large sizes with --perQpCeilingGbps=49 and reports the
single-QP sustained throughput vs the 49.02 Gbps ceiling (convergence as the
per-message floor amortizes), plus one no-ceiling point showing the ~7x
overestimate the ceiling corrects. The ib_write_bw *ramp* (Hill form, MAPE
19.8 %) is TX-depth-128 pipeline dynamics, NOT a calibration target (per the
calib JSON); only the ceiling is. Small sizes undershoot because the 1.5 us
per-message floor is not yet amortized -- the same structural floor as the
ib_write_lat sweep.

Run:  python3 examples/gpu-cluster/roce_p2p_bw_sweep.py
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CFG = "configs/protocol_configs/p2p-roce.cfg"

# ib_write_bw per-QP ceiling (asymptote) from roce_p2p_calib.json.
CEILING_GBPS = 49.02  # bandwidth_fit.ceiling_gbps
# Large sizes (>= the 4KB knee) where the per-QP ceiling binds and the per-msg
# floor amortizes. 64KB still carries ~1.5us floor -> under-shoots; by 16MB the
# floor is negligible -> converges to the ceiling.
SIZES = [65536, 262144, 1048576, 4194304, 16777216, 67108864, 268435456]
NO_CEILING_SIZE = 268435456  # show the ~7x overestimate the ceiling corrects


def run_ns3(size: int, ceiling: float | None) -> float | None:
    arg = (
        f"gpu-cluster-sim --numNodes=2 --gpusPerNode=2 "
        f"--interNodeFabricType=RoCE --interNodeTopology=tor "
        f"--interNodeBandwidth=400 --interNodeLatency=100 "
        + (f"--perQpCeilingGbps={ceiling} " if ceiling else "")
        + f"--protocolConfig={CFG} --dataSize={size}"
    )
    try:
        p = subprocess.run(
            ["./ns3", "run", arg], cwd=REPO, capture_output=True, text=True, timeout=300
        )
    except subprocess.TimeoutExpired:
        return None
    for line in p.stdout.splitlines():
        if line.strip().startswith("simTimeUs"):
            try:
                return float(line.split("=", 1)[1].strip())
            except (ValueError, IndexError):
                return None
    return None


def tput_gbps(size_bytes: int, sim_us: float) -> float:
    return size_bytes * 8 / (sim_us * 1e-6) / 1e9


def main():
    rows = []
    print(f"{'size (B)':>12} {'simTimeUs':>10} {'ns3 Gbps':>9} {'ceiling':>8} {'err%':>6}")
    print("-" * 52)
    for sz in SIZES:
        sim = run_ns3(sz, CEILING_GBPS)
        if sim is None:
            print(f"{sz:>12} FAILED")
            continue
        g = tput_gbps(sz, sim)
        err = (g - CEILING_GBPS) / CEILING_GBPS * 100
        rows.append(dict(size=sz, sim_us=sim, gbps=g, err_pct=err))
        print(f"{sz:>12} {sim:>10.1f} {g:>9.2f} {CEILING_GBPS:>8.2f} {err:>6.1f}")

    # Convergence MAPE: sizes >= 64MB where the pacer has bound (>= 8 chunks of
    # the 8MB bulk coalesce -> the inter-chunk ceiling gap dominates). Below 64MB
    # the transfer is 1-7 chunks: the per-QP ceiling paces INTER-chunk gaps, so a
    # single/few-chunk transfer is not yet paced -> line-rate-ish (the transition
    # zone; the blog's sub-ceiling small-size regime is msg-issue-limited, which
    # ns-3 does not model -- the same structural gap as the ib_write_lat sweep).
    conv = [r for r in rows if r["size"] >= 64 * (1 << 20)]
    mape = sum(abs(r["err_pct"]) for r in conv) / len(conv) if conv else float("inf")
    trans = [r for r in rows if r["size"] < 64 * (1 << 20)]

    no_ceiling_sim = run_ns3(NO_CEILING_SIZE, None)
    no_ceiling_g = tput_gbps(NO_CEILING_SIZE, no_ceiling_sim) if no_ceiling_sim else None

    print("\n" + "=" * 60)
    print("ib_write_bw per-QP ceiling validation (ConnectX-7 400G)")
    print("=" * 60)
    print(f"  ceiling (calib bandwidth_fit.ceiling_gbps): {CEILING_GBPS} Gbps")
    print(f"  converged MAPE (sizes >= 64MB, pacer bound): {mape:.2f} %")
    if trans:
        print(f"  transition zone (< 64MB): {len(trans)} sizes over-estimate -- the")
        print(f"    per-QP ceiling paces INTER-chunk gaps, so 1-7-chunk (8MB-coalesced)")
        print(f"    transfers are not yet paced; the blog's sub-ceiling small-size")
        print(f"    regime is msg-issue-limited (not modeled, like ib_write_lat).")
    if no_ceiling_g is not None:
        print(f"  {NO_CEILING_SIZE // (1 << 20)}MB WITHOUT ceiling: {no_ceiling_g:.1f} Gbps"
              f"  (line rate; {no_ceiling_g / CEILING_GBPS:.1f}x the per-QP ceiling)")
        with_ceiling = next((r["gbps"] for r in rows if r["size"] == NO_CEILING_SIZE), None)
        if with_ceiling:
            print(f"  {NO_CEILING_SIZE // (1 << 20)}MB WITH    ceiling: {with_ceiling:.2f} Gbps  "
                  f"(~{CEILING_GBPS:.0f}G per-QP, the ceiling corrects the {no_ceiling_g/CEILING_GBPS:.1f}x overestimate)")

    out = {
        "source": "share_data/zhuyu/RoCEv2_benchmark_data.md (ib_write_bw per-QP ceiling)",
        "model": "ns-3 RoCE P2P, tor, 1 flowKey==1 QP, large pipelined transfer",
        "ceiling_gbps_anchor": CEILING_GBPS,
        "ramp_note": "ib_write_bw Hill-form ramp (MAPE 19.8%) is TX-depth-128 pipeline "
                     "dynamics, NOT a calibration target (per calib JSON); only the ceiling is.",
        "converged_mape_pct_sizes_ge_64MB": mape,
        "transition_zone_note": "sizes < 64MB over-estimate: the per-QP ceiling paces "
                                 "INTER-chunk gaps, so 1-7-chunk (8MB-coalesced) transfers are "
                                 "not yet paced; the blog's sub-ceiling small-size regime is "
                                 "msg-issue-limited (not modeled, like ib_write_lat).",
        "rows": rows,
        "no_ceiling": {"size": NO_CEILING_SIZE, "gbps": no_ceiling_g},
    }
    out_path = Path(__file__).with_suffix(".json")
    out_path.write_text(json.dumps(out, indent=2))
    print(f"\nWrote {out_path}")


if __name__ == "__main__":
    main()
