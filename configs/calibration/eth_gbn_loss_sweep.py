#!/usr/bin/env python3
"""
ns-3 go-back-N vs selective-repeat loss-recovery sweep, compared against the
SRNIC (NSDI'23) Figure 11 measurement (ConnectX-6 GBN vs SRNIC selective
repeat, 100G, perftest 4 KB messages, CC disabled, FPGA-injected uniform
random loss). Reference: configs/calibration/srnic_fig11_cx6_reference.csv.

Two ns-3 arms, identical except for the recovery mode:
  gobackn -> configs/protocol_configs/p2p-roce-lossy.cfg       (CX-6 analogue)
  sack    -> configs/protocol_configs/p2p-roce-lossy-sack.cfg  (SRNIC analogue)

Testbed mapping (see the cfg headers for the full rationale):
  --numNodes=2 --gpusPerNode=2 (gpn>=2 REQUIRED: m_localRankCount>1 guard)
  --interNodeFabricType=RoCE --interNodeTopology=tor --interNodeBandwidth=100
  --bulkChunkSize=4096 (perftest 4 KB messages) --ber=0 (loss only)
  --soTxWindow: ACK-clocked TX window, per arm. SACK: w=16 = wire BDP
    (RTT ~= 2 x 1 us prop + 2 x 328 ns serialization + ACK return ~= 4.8 us;
    100G x 4.8 us ~= 60 KB ~= 15 x 4 KB chunks) -- selective repeat has no
    rewind, so the window only bounds in-flight. GBN: --soGbnStrict=1 (true
    go-back-N receive: in-order-only acceptance, OOO discarded with no ACK,
    one NAK per gap) + w=64 -- under strict semantics the window IS the
    rewind span, calibrated against the CX-6 anchor within the physical
    bounds [BDP=16, testbed tx_depth=128] (see the ARMS comment).
  --llrRetryTimeout=500us + --llrTimeoutRetransmit=1 (both arms): with the
    window, LLR retry timers arm at actual (window-gated) TX time instead of
    the old synchronous t~=0 dispatch, so an RTT-scale timeout is finally
    sound. 500 us ~= 100 x RTT mirrors real RNIC RTO >> RTT; shorter values
    (100-200 us) inflate GBN retransmissions under blanket load (spurious
    timeouts) without a principled basis. The timeout is the only recovery
    for a lost NACK/NAK and for a lost per-packet ACK (a lost ACK parks one
    window slot until the timer re-sends and the dup delivery re-ACKs).

The comparison is NORMALIZED (G(p)/G(0) per curve): the ns-3 absolute goodput
(~53 Gbps: tor 2-link serialization + per-4KB-chunk headers) intentionally
differs from the single-link 90-97 Gbps testbed plateau. What the experiment
tests is the RECOVERY-MODE degradation shape, not the absolute rate.

Run:  python3 configs/calibration/eth_gbn_loss_sweep.py
Writes configs/calibration/eth_gbn_loss_sweep.json + prints the table.
"""

from __future__ import annotations

import csv
import json
import statistics
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BIN = REPO / "build/scratch/ns3.47-gpu-cluster-sim-debug"
REFERENCE = Path(__file__).with_name("srnic_fig11_cx6_reference.csv")
OUT_JSON = Path(__file__).with_name("eth_gbn_loss_sweep.json")

DATA_SIZE = 16 * 1024 * 1024  # 16 MiB, ~4300 x 4 KB chunks
LOSS_RATES = [0.0, 1e-6, 1e-5, 1e-4, 1e-3, 2e-3, 5e-3, 1e-2, 2e-2]
SEEDS = [1, 2, 3]

TX_WINDOW = 16  # SACK arm: BDP-scale chunks (see docstring)
GBN_TX_WINDOW = 64  # GBN arm: strict-GBN rewind span, see ARMS comment below
RETRY_TIMEOUT_NS = 500_000  # ~100 x RTT; sound now that timers arm at TX time

# Per-arm knobs beyond the cfg:
#   gobackn runs with --soGbnStrict=1 (true go-back-N receive semantics:
#     in-order-only acceptance, OOO arrivals discarded with no ACK, one NAK
#     per gap like an IB responder). Under strict semantics the TX window IS
#     the rewind span (instantaneous dispatch => every dispatched-unACKed
#     chunk is retransmitted AND its queued original still burns wire time),
#     so it is calibrated against the CX-6 anchor within the physical bounds
#     [wire BDP = 16, testbed tx_depth = 128]: w=64 (3-seed norms
#     0.919/0.725/0.580/0.383 vs CX-6 0.933/0.744/0.467/0.278; w=16 is
#     optimistic at high loss, w=128 = tx_depth pessimistic in the middle).
#     The CX-6 curve itself reverse-solves to a per-drop waste of 70-130
#     packets (G = 1/(1+n*p)) -- between BDP and tx_depth, matching w=64.
#   sack keeps w=16 (BDP): selective repeat has no rewind, the window only
#     bounds in-flight, and the BDP-derived value already tracks SRNIC
#     within ~5 pts everywhere.
ARMS = {
    "gobackn": dict(
        cfg="configs/protocol_configs/p2p-roce-lossy.cfg",
        reference_curve="cx6",
        tx_window=GBN_TX_WINDOW,
        extra_args=["--soGbnStrict=1"],
    ),
    "sack": dict(
        cfg="configs/protocol_configs/p2p-roce-lossy-sack.cfg",
        reference_curve="srnic",
        tx_window=TX_WINDOW,
        extra_args=[],
    ),
}


def run_ns3(spec: dict, plr: float, seed: int) -> dict | None:
    cmd = [
        str(BIN),
        "--numNodes=2", "--gpusPerNode=2",
        "--interNodeFabricType=RoCE", "--interNodeTopology=tor",
        "--interNodeBandwidth=100", "--interNodeLatency=1000",
        f"--llrRetryTimeout={RETRY_TIMEOUT_NS}", "--llrRetryLimit=64",
        "--llrBufferSize=8192", "--llrTimeoutRetransmit=1",
        f"--soTxWindow={spec['tx_window']}",
        *spec["extra_args"],
        "--ber=0", f"--packetLossRate={plr}",
        "--bulkChunkSize=4096",
        f"--protocolConfig={spec['cfg']}",
        f"--dataSize={DATA_SIZE}",
        f"--RngRun={seed}",
    ]
    try:
        p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        return None
    out = {}
    for line in p.stdout.splitlines():
        for key in ("status", "simTimeNs", "retransmittedPackets", "permanentLosses"):
            if line.startswith(key + "="):
                out[key] = line.split("=", 1)[1].strip()
    if out.get("status") != "complete" or "simTimeNs" not in out:
        return None
    out["simTimeNs"] = int(out["simTimeNs"])
    return out


def load_reference() -> dict[str, dict[float, float]]:
    ref: dict[str, dict[float, float]] = {}
    with open(REFERENCE) as f:
        for row in csv.DictReader(r for r in f if not r.startswith("#")):
            ref.setdefault(row["curve"], {})[float(row["loss_rate"])] = float(row["normalized"])
    return ref


def main() -> None:
    ref = load_reference()
    results: dict[str, list[dict]] = {}
    for arm, spec in ARMS.items():
        rows = []
        base_ns: float | None = None
        for plr in LOSS_RATES:
            times, retx, ploss, failed = [], [], [], 0
            for seed in SEEDS:
                r = run_ns3(spec, plr, seed)
                if r is None:
                    failed += 1
                    continue
                times.append(r["simTimeNs"])
                retx.append(int(r.get("retransmittedPackets", 0)))
                ploss.append(int(r.get("permanentLosses", 0)))
            if not times:
                print(f"{arm} plr={plr:g}: ALL {len(SEEDS)} RUNS FAILED")
                rows.append(dict(plr=plr, failed=failed))
                continue
            mean_ns = statistics.mean(times)
            goodput = DATA_SIZE * 8 / mean_ns  # Gbps (bits/ns)
            if plr == 0.0:
                base_ns = mean_ns
            norm = base_ns / mean_ns if base_ns else float("nan")
            ref_norm = ref[spec["reference_curve"]].get(plr)
            rows.append(dict(
                plr=plr, mean_ns=mean_ns, goodput_gbps=round(goodput, 2),
                normalized=round(norm, 3),
                reference_normalized=ref_norm,
                retransmitted=retx, permanent_losses=ploss,
                sim_time_ns=times, failed=failed,
            ))
            refs = f" ref({spec['reference_curve']})={ref_norm:.3f}" if ref_norm is not None else ""
            print(f"{arm:8s} plr={plr:<7g} goodput={goodput:6.2f}G "
                  f"norm={norm:.3f}{refs}  retx={retx} permLoss={ploss}")
        results[arm] = rows

    print("\n" + "=" * 74)
    print("NORMALIZED GOODPUT G(p)/G(0): ns-3 vs SRNIC Fig.11 (measured)")
    print("=" * 74)
    print(f"{'loss':>8} | {'ns3 gbn':>8} {'cx6 meas':>9} | {'ns3 sack':>9} {'srnic meas':>10}")
    for i, plr in enumerate(LOSS_RATES):
        if plr == 0.0:
            continue
        g = results["gobackn"][i]
        s = results["sack"][i]
        def fmt(row, key):
            v = row.get(key)
            return f"{v:.3f}" if isinstance(v, float) else "  -- "
        print(f"{plr:>8g} | {fmt(g,'normalized'):>8} {fmt(g,'reference_normalized'):>9} "
              f"| {fmt(s,'normalized'):>9} {fmt(s,'reference_normalized'):>10}")

    OUT_JSON.write_text(json.dumps(dict(
        data_size=DATA_SIZE, seeds=SEEDS,
        arms={a: dict(cfg=s["cfg"], tx_window=s["tx_window"],
                      extra_args=s["extra_args"]) for a, s in ARMS.items()},
        retry_timeout_ns=RETRY_TIMEOUT_NS,
        reference=str(REFERENCE.name), results=results), indent=2))
    print(f"\nwrote {OUT_JSON}")


if __name__ == "__main__":
    main()
