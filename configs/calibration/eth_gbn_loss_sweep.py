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
  --soTxWindow=16 (both arms): ACK-clocked BDP-scale TX window. The window is
    sized from wire BDP, not curve-fit: RTT ~= 2 x 1 us prop + 2 x 328 ns
    serialization + ACK return ~= 4.8 us; 100G x 4.8 us ~= 60 KB ~= 15 x 4 KB
    chunks. Because chunk dispatch into the device queue is instantaneous,
    window == in-flight == the GBN go-back blanket size, so the window must be
    BDP-scale (a real NIC's tx_depth=128 caps posted WQEs, but its go-back
    amount is still the wire BDP). The blanket cascade is subcritical when
    R = window x plr < 1 (w=16 @ 2e-2 -> R = 0.32; w=128 @ 1e-2 -> R = 1.28
    explodes to 111 ms with 3283 retry-limit exhaustions).
  --llrRetryTimeout=500us + --llrTimeoutRetransmit=1 (both arms): with the
    window, LLR retry timers arm at actual (window-gated) TX time instead of
    the old synchronous t~=0 dispatch, so an RTT-scale timeout is finally
    sound. 500 us ~= 100 x RTT mirrors real RNIC RTO >> RTT; shorter values
    (100-200 us) inflate GBN retransmissions under blanket load (spurious
    timeouts: 2407 vs 1539 retx @ 2e-2) without a principled basis. The
    timeout is the only recovery for a lost NACK (SACK arm) and for a lost
    per-packet ACK (both arms; a lost ACK parks one window slot until the
    timer re-sends and the dup delivery re-ACKs).

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

TX_WINDOW = 16  # BDP-scale chunks (see docstring); identical for both arms
RETRY_TIMEOUT_NS = 500_000  # ~100 x RTT; sound now that timers arm at TX time

ARMS = {
    "gobackn": dict(
        cfg="configs/protocol_configs/p2p-roce-lossy.cfg",
        reference_curve="cx6",
    ),
    "sack": dict(
        cfg="configs/protocol_configs/p2p-roce-lossy-sack.cfg",
        reference_curve="srnic",
    ),
}


def run_ns3(cfg: str, plr: float, seed: int) -> dict | None:
    cmd = [
        str(BIN),
        "--numNodes=2", "--gpusPerNode=2",
        "--interNodeFabricType=RoCE", "--interNodeTopology=tor",
        "--interNodeBandwidth=100", "--interNodeLatency=1000",
        f"--llrRetryTimeout={RETRY_TIMEOUT_NS}", "--llrRetryLimit=64",
        "--llrBufferSize=8192", "--llrTimeoutRetransmit=1",
        f"--soTxWindow={TX_WINDOW}",
        "--ber=0", f"--packetLossRate={plr}",
        "--bulkChunkSize=4096",
        f"--protocolConfig={cfg}",
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
                r = run_ns3(spec["cfg"], plr, seed)
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
        data_size=DATA_SIZE, seeds=SEEDS, arms={a: s["cfg"] for a, s in ARMS.items()},
        tx_window=TX_WINDOW, retry_timeout_ns=RETRY_TIMEOUT_NS,
        reference=str(REFERENCE.name), results=results), indent=2))
    print(f"\nwrote {OUT_JSON}")


if __name__ == "__main__":
    main()
