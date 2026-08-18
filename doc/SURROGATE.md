# Surrogate model

ProtoBridge ships a latency surrogate so broad design spaces can be screened
without running the full ns-3 sweep. It is grounded in the H200 NVLink4
reference (see [CALIBRATION.md](CALIBRATION.md)).

## Theory-derived surrogate — `surrogate/theory/`

`whitebox_surrogate_v2.py` is a first-principles physical model. The full
derivation lives in [`surrogate/theory/WHITEBOX_SURROGATE.md`](../surrogate/theory/WHITEBOX_SURROGATE.md);
the core design rule keeps fixed latency separate from serialized packet
work:

```
L_hat = S_protocol                              # protocol startup (LL/LL128 ~15us, SIMPLE ~65us)
        + steps * T_fixed                        # per-step fixed (hop × link latency + sw overhead)
        + steps * T_serial * Phi_credit          # serialized bytes / effective_bw
                         * Phi_fec               #   × FEC bandwidth overhead (K/N)
                         * Phi_share             #   × contention share (WFQ)
        + T_retransmission                       # GBN/SACK retransmission tail
        + T_reload                               # LLR buffer reload
        + T_fec_latency                         # FEC encode + decode latency
```

H200 factory functions: `make_h200_ring_theory()`, `make_h200_tree_theory()`,
`make_h200_nvls_theory()`, `make_1024_optical_fattree_theory()`.

```python
import whitebox_surrogate_v2 as wb
s = wb.make_h200_ring_theory()
lat_us = s.predict(data_bytes=1<<20, num_gpus=4, algo="ring", credits=32, ber=0)
```

Because it is a physical derivation (no firmware/arbiter/VOQ overhead), it is a
**lower bound** on the ns-3 measurement — typically 0.6–0.75× the simulated
latency. Use it for ordering and trend analysis, not absolute prediction.

## Tests

```bash
python3 -m pytest surrogate/test/ -v
```

- `test_theory.py` — the theory model returns finite, monotone-in-size
  latencies within a 2× physical band of the ns-3 baseline (generous, since
  it is an uncalibrated lower bound).
