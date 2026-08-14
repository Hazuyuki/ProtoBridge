"""Test the regression-calibrated analytical surrogate against the H200 ring4 baseline.

The analytical surrogate is a piecewise (small-size / large-size) regression
fit with a FEC/retry amplification overlay. Calibrated on the H200 NVLink4
ring4 allreduce baseline, its documented accuracy is ~0.9% mean / ~2.7% max
APE (27 points, 64 KiB–1 GiB). We assert the mean APE stays below 2%.
"""
import json
import os

import analytical_surrogate as an

_CAL = os.path.join(os.path.dirname(__file__), "..", "calibration",
                    "h200_ring4_ar_baseline.json")


def _load_baseline():
    with open(_CAL) as f:
        data = json.load(f)
    for r in data:
        r["ber"] = 0
        r["fecEnabled"] = 0
        r["llrEnabled"] = 0
        r["algorithm"] = "ring"
        r["numGpus"] = 4
        r["collective"] = "allreduce"
    return data


def test_calibrate_then_validate_within_band():
    s = an.AnalyticalSurrogate()
    data = _load_baseline()
    s.calibrate(data)
    assert s.calibrated, "surrogate did not report calibrated=True"
    val = s.validate(data)
    assert val["total"] == len(data)
    # Documented accuracy: avg ~0.9%, max ~2.7%. Allow a small margin.
    assert val["avg_error_pct"] < 2.0, f"avg APE {val['avg_error_pct']:.2f}% > 2%"
    assert val["max_error_pct"] < 5.0, f"max APE {val['max_error_pct']:.2f}% > 5%"
    assert val["within_pct"] >= 90.0


def test_predict_latency_finite_at_representative_sizes():
    s = an.AnalyticalSurrogate()
    s.calibrate(_load_baseline())
    for size in (1 << 16, 1 << 20, 1 << 26, 1 << 30):
        r = s.predict_latency_us(size)
        p50 = r["p50"]
        assert p50 == p50 and p50 > 0.0, f"size {size}: non-finite p50 {p50}"
        assert p50 > 0.0


def test_latency_monotonic_in_size():
    s = an.AnalyticalSurrogate()
    s.calibrate(_load_baseline())
    small = s.predict_latency_us(1 << 16)["p50"]
    large = s.predict_latency_us(1 << 26)["p50"]
    assert large > small
