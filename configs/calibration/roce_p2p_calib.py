#!/usr/bin/env python3
"""
RoCE v2 P2P calibration against an ib_write_bw / ib_write_lat perftest curve.

Data source
-----------
/share_data/zhuyu/RoCEv2_benchmark_data.md  (third-party engineering blog,
kubernetes.recipes, author Luca Berton, 2026-06-15). Environment: NVIDIA
ConnectX-7 400G, RoCEv2 (GID idx 3, IPv4), MTU 4096, RC, TX depth 128,
CQ moderation 1, PCIe relaxed order ON, ibv_wr* API ON.

This data is RAW RDMA (one-sided RDMA write) measured with the perftest
suite. It sits BELOW the NCCL layer: there is no collective algorithm, no
GPU kernel launch, no NCCL persistent-kernel wakeup. It therefore
calibrates the *raw wire + NIC* constants of the RoCE model, NOT the
NCCL-collective timing. The NCCL 40 us startup that the ns-3
RoceProtocolModel carries is a *different layer* and is intentionally NOT
exercised by perftest.

Provenance / honesty
--------------------
- Third-party blog, not peer-reviewed, not the project's A800 hardware.
- No switch model, no PFC/ECN config disclosed, no congestion data.
=> This is a SOFT, externally-anchored sanity calibration of scalar RoCE
   constants, NOT a fidelity verdict on collective simTime.

What it extracts (the calibration-relevant values)
--------------------------------------------------
- per-QP (single-flow) sustained ceiling  ~ 49 Gbps
- wire efficiency                         ~ 0.95-0.99 (8-QP saturation 0.9875)
- per-message fixed latency (doorbell+    ~ 1.45 us  (2-byte ib_write_lat)
  prop+switch+completion, same-switch)
- NIC doorbell / message-issue rate       ~ 8.1 Mmsg/s (2-byte ib_write_bw)
- saturation knee                         ~ 4 KB
- line rate                               400 Gbps

Run:  python3 configs/calibration/roce_p2p_calib.py
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from scipy.optimize import curve_fit

# --------------------------------------------------------------------------
# Measured data (bytes -> Gbps; bytes -> microseconds). From the blog's
# "测量数据汇总" tables. BW = ib_write_bw average Gbps (single QP, TX depth 128);
# LAT = ib_write_lat t_avg microseconds (same switch).
# --------------------------------------------------------------------------
BW_DATA = {  # bytes: Gbps (avg)
    2: 0.13,
    4: 0.27,
    64: 4.18,
    1024: 41.89,
    4096: 48.76,
    65536: 49.07,
    1048576: 49.12,
    8388608: 49.14,
}
LAT_DATA = {  # bytes: us (t_avg)
    2: 1.45,
    64: 1.48,
    1024: 1.62,
    65536: 4.21,
}
MULTI_QP = {1: 49.0, 2: 98.0, 4: 196.0, 8: 395.0}  # QPs -> Gbps (approx)

LINE_RATE_GBPS = 400.0  # ConnectX-7 400G, per-link unidirectional


def _gbps_to_Bps(g: float) -> float:
    return g * 1e9 / 8.0


def _Bps_to_gbps(b: float) -> float:
    return b * 8.0 / 1e9


# --------------------------------------------------------------------------
# Model 1: single-write latency (ib_write_lat).
#   L(S) = a + b * S**c      (a = fixed doorbell+prop+completion; b,c = scaling)
# A single RDMA write with completion is NOT throughput-pipelined, so this is
# the cleanest per-message latency signal.
# --------------------------------------------------------------------------
def lat_model(S, a, b, c):
    return a + b * np.power(S, c)


def fit_latency():
    S = np.array(list(LAT_DATA.keys()), dtype=float)
    L = np.array(list(LAT_DATA.values()), dtype=float)
    p0 = [1.45, 1e-5, 1.0]
    popt, _ = curve_fit(lat_model, S, L, p0=p0, maxfev=20000)
    a, b, c = popt
    pred = lat_model(S, *popt)
    mape = float(np.mean(np.abs(pred - L) / L) * 100.0)
    # Also report the simplest linear-ish anchor: L0 from 2B, R_lat from 2B->64KB.
    L0 = LAT_DATA[2]
    # effective single-write serialization rate between the two largest points:
    # (delta bits) / (delta seconds)  -- latency is in microseconds.
    s_lo, s_hi = 1024, 65536
    R_lat_gbps = (s_hi - s_lo) * 8.0 / ((LAT_DATA[s_hi] - LAT_DATA[s_lo]) * 1e-6) / 1e9
    return dict(a_us=float(a), b=float(b), c=float(c), mape_pct=mape,
                L0_us=L0, R_lat_gbps=float(R_lat_gbps))


# --------------------------------------------------------------------------
# Model 2: sustained throughput (ib_write_bw, TX depth 128 pipelined).
#   BW(S) = R / (1 + (S0/S)**d)        (Hill / sigmoid saturation to ceiling R)
# The full ramp shape is perftest-pipeline-specific (TX depth 128 completion
# dynamics) and does NOT map 1:1 to ns-3 collective behavior; only the
# ceiling R and the message-issue rate are calibration-relevant. We fit the
# Hill form for completeness and report honest MAPE.
# --------------------------------------------------------------------------
def bw_model(S, R, S0, d):
    return R / (1.0 + np.power(S0 / S, d))


def fit_bandwidth():
    S = np.array(list(BW_DATA.keys()), dtype=float)
    B = np.array(list(BW_DATA.values()), dtype=float)
    p0 = [49.0, 700.0, 1.5]
    popt, _ = curve_fit(bw_model, S, B, p0=p0, maxfev=40000)
    R, S0, d = popt
    pred = bw_model(S, *popt)
    mape = float(np.mean(np.abs(pred - B) / B) * 100.0)
    # Direct extraction (model-independent):
    ceiling = float(np.mean([BW_DATA[k] for k in (4096, 65536, 1048576, 8388608)]))
    knee = next(s for s in sorted(BW_DATA) if BW_DATA[s] >= 0.9 * ceiling)
    # Tiny-message regime: BW ~ S * msg_rate (doorbell-limited, TX depth not yet
    # amortizing). Verify on 2B/4B/64B (strictly proportional) and report MAPE.
    tiny = [2, 4, 64]
    msg_rate_Mps = np.array([BW_DATA[s] / s for s in tiny]) * 1e9 / 8.0 * 1e-6  # Gbps/B -> Mmsg/s
    mr_mean = float(np.mean(msg_rate_Mps))
    pred_tiny = np.array([s * mr_mean * 1e6 * 8.0 / 1e9 for s in tiny])  # back to Gbps
    meas_tiny = np.array([BW_DATA[s] for s in tiny])
    tiny_mape = float(np.mean(np.abs(pred_tiny - meas_tiny) / meas_tiny) * 100.0)
    return dict(R_gbps=float(R), S0=float(S0), d=float(d), mape_pct=mape,
                ceiling_gbps=ceiling, knee_bytes=int(knee),
                msg_rate_Mps_mean=mr_mean,
                msg_rate_Mps_points=[round(x, 2) for x in msg_rate_Mps],
                tiny_regime_mape_pct=tiny_mape)


def derive_wire_efficiency():
    # 8-QP saturation ~ 395 Gbps out of 400G line rate.
    eff_8qp = MULTI_QP[8] / LINE_RATE_GBPS
    # Per-QP ceiling as a fraction of line rate (single-QP / 400G):
    per_qp_frac = MULTI_QP[1] / LINE_RATE_GBPS
    # QPs needed to reach ~line rate:
    qps_to_saturate = int(round(LINE_RATE_GBPS / MULTI_QP[1]))
    return dict(eff_8qp=float(eff_8qp), per_qp_ceiling_gbps=MULTI_QP[1],
                per_qp_ceiling_frac=per_qp_frac,
                qps_to_saturate=qps_to_saturate,
                model_wire_eff_default=0.95,
                note="8-QP saturation 0.9875 validates 0.95 as slightly conservative")


# --------------------------------------------------------------------------
# ns-3 RoceProtocolModel + So-link defaults (what we are calibrating toward).
# --------------------------------------------------------------------------
NS3_DEFAULTS = dict(
    startup_delay_ns=40000,        # RoceProtocolModel::StartupDelayNs  (NCCL-launch layer)
    per_step_latency_ns=800,       # RoceProtocolModel::PerStepLatencyNs
    chunk_size_bytes=512 * 1024,   # RoceProtocolModel::ChunkSize
    wire_efficiency=0.95,          # RoceProtocolModel::WireEfficiency
    inter_node_link_latency_ns=10000,  # --interNodeLatency (gpu-cluster-sim.cc)
    inter_node_bandwidth_gbps=400,     # --interNodeBandwidth
    per_qp_ceiling_gbps=None,          # NOT MODELED (single flow = full 400G)
    per_message_doorbell_ns=None,      # NOT MODELED (startup=40us covers NCCL, not RDMA doorbell)
    source="src/gpu-cluster/model/roce-protocol-model.cc + scratch/gpu-cluster-sim.cc defaults",
)


def main():
    lat = fit_latency()
    bw = fit_bandwidth()
    eff = derive_wire_efficiency()

    print("=" * 72)
    print("RoCE P2P calibration vs ib_write_bw / ib_write_lat perftest curve")
    print("Source: /share_data/zhuyu/RoCEv2_benchmark_data.md (ConnectX-7 400G)")
    print("=" * 72)

    print("\n[1] Single-write latency L(S)=a+b*S**c (ib_write_lat, same switch)")
    print(f"    fixed overhead a        = {lat['a_us']:.3f} us   (meas 2B = {lat['L0_us']:.2f} us)")
    print(f"    scaling b, c            = {lat['b']:.3e}, {lat['c']:.3f}")
    print(f"    single-write ser. rate  = {lat['R_lat_gbps']:.1f} Gbps (2B->64KB slope)")
    print(f"    fit MAPE                = {lat['mape_pct']:.2f} %  (4 pts)")

    print("\n[2] Sustained throughput BW(S)=R/(1+(S0/S)**d) (ib_write_bw, 1 QP)")
    print(f"    per-QP ceiling R        = {bw['R_gbps']:.2f} Gbps   (direct ceiling {bw['ceiling_gbps']:.2f})")
    print(f"    knee S0, exponent d     = {bw['S0']:.1f} B, {bw['d']:.3f}")
    print(f"    saturation knee (90%R)   = {bw['knee_bytes']} B")
    print(f"    NIC msg-issue rate       = {bw['msg_rate_Mps_mean']:.2f} Mmsg/s "
          f"(pts {bw['msg_rate_Mps_points']})")
    print(f"    tiny-regime BW=S*msg_rate MAPE = {bw['tiny_regime_mape_pct']:.2f} % (2B/4B/64B)")
    print(f"    fit MAPE                = {bw['mape_pct']:.2f} %  (8 pts)  "
          f"[ramp is perftest-TX-depth-128 specific, ceiling is the calib signal]")

    print("\n[3] Wire efficiency / multi-QP scaling")
    print(f"    8-QP saturation         = {eff['eff_8qp']:.4f}  ({MULTI_QP[8]}/{LINE_RATE_GBPS})")
    print(f"    per-QP ceiling          = {eff['per_qp_ceiling_gbps']} Gbps "
          f"({eff['per_qp_ceiling_frac']:.3f} of line rate)")
    print(f"    QPs to saturate 400G    = {eff['qps_to_saturate']}")
    print(f"    model wire_eff default  = {eff['model_wire_eff_default']}  -> {eff['note']}")

    print("\n[4] ns-3 RoCE defaults vs calibrated (delta table)")
    rows = [
        ("per-QP single-flow ceiling", "NOT MODELED (flow=400G)",
         f"{eff['per_qp_ceiling_gbps']} Gbps", "ADD: cap single So flow at ~49G"),
        ("per-message doorbell latency", "NOT MODELED (startup=40us NCCL)",
         f"{lat['L0_us']:.2f} us", "raw-RDMA mode != NCCL; doorbell ~1.45us"),
        ("NCCL startup delay", f"{NS3_DEFAULTS['startup_delay_ns']/1000:.0f} us",
         "N/A (perftest bypasses NCCL)", "keep for collective mode; do NOT apply to raw P2P"),
        ("So link propagation", f"{NS3_DEFAULTS['inter_node_link_latency_ns']/1000:.0f} us",
         f"{lat['L0_us']:.2f} us (doorbell+prop+completion)",
         "10us too high for same-switch; prop alone ~0.3-0.5us"),
        ("wire efficiency", f"{NS3_DEFAULTS['wire_efficiency']}",
         f"{eff['eff_8qp']:.3f}", "0.95 slightly conservative; bump ~0.98"),
        ("line rate", f"{NS3_DEFAULTS['inter_node_bandwidth_gbps']} Gbps",
         f"{LINE_RATE_GBPS} Gbps", "match"),
    ]
    hdr = f"    {'param':<32} {'ns-3 now':<30} {'calibrated':<26} action"
    print(hdr)
    print("    " + "-" * (len(hdr) - 4))
    for p, now, cal, act in rows:
        print(f"    {p:<32} {now:<30} {cal:<26} {act}")

    out = {
        "source": "share_data/zhuyu/RoCEv2_benchmark_data.md",
        "source_type": "third-party engineering blog (not peer-reviewed, not A800 HW)",
        "env": {"nic": "ConnectX-7 400G", "proto": "RoCEv2 GID3 IPv4", "mtu": 4096,
                "transport": "RC", "tx_depth": 128},
        "line_rate_gbps": LINE_RATE_GBPS,
        "calibrated": {
            "per_qp_ceiling_gbps": eff["per_qp_ceiling_gbps"],
            "per_qp_ceiling_frac_of_line": eff["per_qp_ceiling_frac"],
            "qps_to_saturate_line": eff["qps_to_saturate"],
            "wire_efficiency_8qp": eff["eff_8qp"],
            "wire_efficiency_recommended": round((eff["eff_8qp"] + 0.95) / 2, 4),
            "per_message_fixed_latency_us": lat["L0_us"],
            "single_write_serialization_rate_gbps": lat["R_lat_gbps"],
            "nic_msg_issue_rate_Mps": bw["msg_rate_Mps_mean"],
            "saturation_knee_bytes": bw["knee_bytes"],
            "latency_fit": lat,
            "bandwidth_fit": {k: v for k, v in bw.items() if k != "msg_rate_Mps_points"},
        },
        "ns3_defaults": NS3_DEFAULTS,
        "applicability": {
            "usable_for": ["per-QP single-flow ceiling", "wire_efficiency",
                           "per-message doorbell latency", "saturation knee"],
            "NOT_usable_for": ["collective simTime fidelity (raw P2P, not AR/AG/A2A)",
                               "multi-node/multi-hop scaling (2-node same-switch)",
                               "DCQCN/PFC/ECN (no congestion data)"],
        },
    }
    out_path = Path(__file__).with_suffix(".json")
    out_path.write_text(json.dumps(out, indent=2))
    print(f"\n[5] Wrote {out_path}")
    print("\nVerdict: data calibrates the raw-RDMA scalar constants of the RoCE")
    print("model (per-QP ceiling 49G, wire_eff ~0.98, doorbell ~1.45us, knee ~4KB).")
    print("It does NOT verify collective simTime (raw P2P != NCCL collectives).")


if __name__ == "__main__":
    main()
