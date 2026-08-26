#!/usr/bin/env python3
"""
ns-3 RoCE v2 P2P single-transfer sweep vs the ib_write_lat perftest curve.

For each {startup, interNodeLatency, switchCutThroughDelay} combo (interNodeBandwidth
fixed at 400 -- derived from the ib_write_lat 64 KB slope: the GPU->switch->GPU path
serializes on 2 P2P links, so 2x serialization at 400 Gbps gives the ~199 Gbps
single-write rate; see configs/calibration/roce_p2p_calib.json
single_write_serialization_rate_gbps), run the p2p-roce.cfg stencil across the
ib_write_lat sizes and compute MAPE vs the measured curve. Reports the best-fit
constants and the comparison table.

Model (tor single-shared-leaf, 2 P2P links GPU->switch->GPU, cut-through switch):
  simTimeUs(S) ~= startup + 2*interNodeLatency + switchCutThroughDelay
                 + 2 * ceil(S/0.95) * 8 / interNodeBandwidth   (2-link serialization)
ib_write_lat(S) = 1.45 + 0.001654 * S**0.669  (sublinear NIC pipeline; ns-3 is linear
in S, so the 1-4 KB transition undershoots where ib_write_lat is sublinear -- the same
divergence the analytical surrogate shows).

Run:  python3 configs/calibration/roce_p2p_ns3_sweep.py
"""

from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CFG_TEMPLATE = """\
[stack]
profile = configs/protocol_profiles/h200-ll128.profile

[op]
param.N = numGpus
replicate.gpu = 0 .. 0
startup = {startup}
per_step_delay = 0
transfer.0.kind  = P2P
transfer.0.src   = 0
transfer.0.dst   = N - 1
transfer.0.vc    = 0
transfer.0.bytes = dataSize
complete = all
"""

# ib_write_lat measured curve (bytes -> us), from roce_p2p_calib.json source blog.
# 4096 is not a blog lat point; use the fitted L(S)=1.45+0.001654*S**0.669 value.
IB_WRITE_LAT = {2: 1.45, 64: 1.48, 1024: 1.62, 4096: 1.88, 65536: 4.21}
SIZES = sorted(IB_WRITE_LAT.keys())

# Grid: bw fixed at 400 (derived from the 64 KB slope); sweep the floor knobs.
INTER_NODE_BANDWIDTH = 400
STARTUPS = [950, 1050, 1150]
LATENCIES = [100, 150, 200]
CUT_THROUGHS = [100, 200]


def run_ns3(cfg_path: Path, size: int, lat: int, ct: int) -> float | None:
    arg = (
        f"gpu-cluster-sim --numNodes=2 --gpusPerNode=2 "
        f"--interNodeFabricType=RoCE --interNodeTopology=tor "
        f"--interNodeBandwidth={INTER_NODE_BANDWIDTH} --interNodeLatency={lat} "
        f"--switchCutThroughDelay={ct} "
        f"--protocolConfig={cfg_path} --dataSize={size}"
    )
    try:
        p = subprocess.run(
            ["./ns3", "run", arg],
            cwd=REPO,
            capture_output=True,
            text=True,
            timeout=120,
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


def mape(pred: dict[int, float], meas: dict[int, float]) -> float:
    errs = [abs(pred[s] - meas[s]) / meas[s] for s in meas if s in pred and pred[s] is not None]
    return 100.0 * sum(errs) / len(errs) if errs else float("inf")


def main():
    results = []
    best = None
    for startup in STARTUPS:
        for lat in LATENCIES:
            for ct in CUT_THROUGHS:
                with tempfile.NamedTemporaryFile(
                    "w", suffix=".cfg", dir=REPO / "configs/protocol_configs", delete=False
                ) as tf:
                    tf.write(CFG_TEMPLATE.format(startup=startup))
                    tmp_path = Path(tf.name)
                try:
                    pred = {}
                    for sz in SIZES:
                        sim = run_ns3(tmp_path, sz, lat, ct)
                        pred[sz] = sim
                    valid = {s: v for s, v in pred.items() if v is not None}
                    if len(valid) != len(SIZES):
                        results.append(dict(startup=startup, lat=lat, ct=ct,
                                           mape=float("inf"), failed=len(SIZES) - len(valid)))
                        print(f"startup={startup} lat={lat} ct={ct}: FAILED {len(SIZES)-len(valid)}/{len(SIZES)}")
                        continue
                    m = mape(valid, IB_WRITE_LAT)
                    row = dict(startup=startup, lat=lat, ct=ct, mape=m, pred=valid)
                    results.append(row)
                    if best is None or m < best["mape"]:
                        best = row
                    print(f"startup={startup:4d} lat={lat:3d} ct={ct:3d}: "
                          f"MAPE={m:5.2f}%  "
                          + "  ".join(f"{s}:{v:.2f}" for s, v in sorted(valid.items())))
                finally:
                    tmp_path.unlink(missing_ok=True)

    print("\n" + "=" * 78)
    print("BEST FIT vs ib_write_lat")
    print("=" * 78)
    if best:
        b = best
        print(f"  startup={b['startup']}ns interNodeLatency={b['lat']}ns "
              f"switchCutThroughDelay={b['ct']}ns interNodeBandwidth={INTER_NODE_BANDWIDTH}Gbps")
        print(f"  MAPE = {b['mape']:.2f}%")
        print(f"\n  {'size(B)':>8} {'ns3_us':>8} {'ibwl_us':>8} {'err%':>7}")
        for sz in SIZES:
            v = b["pred"][sz]
            print(f"  {sz:>8} {v:>8.2f} {IB_WRITE_LAT[sz]:>8.2f} "
                  f"{abs(v-IB_WRITE_LAT[sz])/IB_WRITE_LAT[sz]*100:>7.1f}")

    out = {
        "source": "share_data/zhuyu/RoCEv2_benchmark_data.md (ib_write_lat)",
        "model": "ns-3 RoCE P2P, tor single-shared-leaf, 2 P2P links, cut-through switch",
        "inter_node_bandwidth_gbps_fixed": INTER_NODE_BANDWIDTH,
        "bw_rationale": "2-link serialization at 400G => ~199G single-write rate (ib_write_lat slope)",
        "ib_write_lat_points": IB_WRITE_LAT,
        "best": best,
        "all_results": sorted(results, key=lambda r: r["mape"])[:10],
    }
    out_path = Path(__file__).with_suffix(".json")
    out_path.write_text(json.dumps(out, indent=2, default=str))
    print(f"\nWrote {out_path}")


if __name__ == "__main__":
    main()
