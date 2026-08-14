"""Test the theory-derived (whitebox) surrogate against the H200 ring4 baseline.

The theory model is a first-principles physical derivation (startup +
serialization + credit round-trip + FEC/retry terms), NOT a regression fit.
It is therefore a lower bound on the ns-3 measurement (it omits firmware
overheads, arbiter jitter, VOQ drain) — so we assert a generous physical
plausibility band (within 2x), not the <1% MAPE that the calibrated
analytical surrogate achieves.
"""
import json
import os

import pytest

import whitebox_surrogate_v2 as wb

_CAL = os.path.join(os.path.dirname(__file__), "..", "calibration",
                    "h200_ring4_ar_baseline.json")


def _baseline():
    with open(_CAL) as f:
        return json.load(f)


def test_predict_returns_finite_positive_float():
    s = wb.make_h200_ring_theory()
    lat = s.predict(1 << 20, 4, "ring", credits=32, ber=0)
    assert isinstance(lat, float)
    assert lat > 0.0
    assert lat == lat  # not NaN


def test_latency_monotonic_in_size():
    """Larger message -> larger latency (the model must capture serialization)."""
    s = wb.make_h200_ring_theory()
    small = s.predict(1 << 16, 4, "ring", credits=32, ber=0)
    large = s.predict(1 << 26, 4, "ring", credits=32, ber=0)
    assert large > small


def test_theory_within_physical_band_of_ns3():
    """The whitebox prediction is within 2x of the ns-3 H200 ring measurement.

    The model is a physical lower bound, so predict <= ns3 is expected; we also
    allow modest over-prediction. Anything outside [0.3x, 2.0x] indicates a
    broken derivation, not merely a missing overhead term.
    """
    s = wb.make_h200_ring_theory()
    baseline = _baseline()
    # Use the 64 MiB point (well into the bandwidth-bound regime, away from the
    # startup-dominated small-message noise floor).
    pt = next(r for r in baseline if r["dataSize"] == 1 << 26 and r["seed"] == 1)
    pred = s.predict(pt["dataSize"], 4, "ring", credits=32, ber=0)
    ratio = pred / pt["simTimeUs"]
    assert 0.3 <= ratio <= 2.0, f"theory {pred:.1f}us vs ns3 {pt['simTimeUs']}us, ratio {ratio:.2f}"
