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


def test_factory_default_lane_counts_preserved():
    """The **overrides refactor must keep the calibrated default lanes."""
    assert wb.make_h200_ring_theory().num_lanes == 4
    assert wb.make_h200_tree_theory().num_lanes == 18
    assert wb.make_h200_nvls_theory().num_lanes == 18


def test_hardware_override_applies():
    """A passed hardware override reaches the constructed surrogate."""
    assert wb.make_h200_ring_theory(num_lanes=8).num_lanes == 8
    assert wb.make_h200_ring_theory(startup_us=7).S_startup == 7
    # tree delegates to ring; its default-18 survives, and a user override wins.
    assert wb.make_h200_tree_theory(num_lanes=12).num_lanes == 12


def test_higher_bandwidth_lowers_latency():
    """ring_bw is the serialized-bytes term: more bandwidth -> less latency."""
    s_default = wb.make_h200_ring_theory()
    s_fast = wb.make_h200_ring_theory(ring_bw_bytes_per_us=200000)  # > default 177000
    lat_default = s_default.predict(1 << 22, 8, "ring", credits=32, ber=0)
    lat_fast = s_fast.predict(1 << 22, 8, "ring", credits=32, ber=0)
    assert lat_fast < lat_default


def test_startup_override_is_additive():
    """startup_us is an additive fixed term: +35us startup -> +~35us latency."""
    base = wb.make_h200_ring_theory().predict(1 << 20, 4, "ring", credits=32, ber=0)
    raised = wb.make_h200_ring_theory(startup_us=50.0).predict(
        1 << 20, 4, "ring", credits=32, ber=0)
    assert abs((raised - base) - 35.0) < 0.5


def test_surrogate_from_wire_derives_h200_bandwidths():
    """One wire-rate input + num_lanes derives all schedule BWs within 2.5%
    of the calibrated H200 values -- formula, not per-bandwidth fit."""
    s = wb.make_surrogate_from_wire(
        b_link_bytes_per_us=25000, num_lanes=18, algo="ring")
    assert abs(s.ring_bw - 177000) / 177000 <= 0.025
    assert abs(s.tree_bw_per_level - 26250) / 26250 <= 0.025
    assert abs(s.nvls_bw - 535500) / 535500 <= 0.025


def test_surrogate_from_wire_reproduces_h200_ring_measurement():
    """The un-calibrated derived profile reproduces the 256MB@8-GPU H200
    AllReduce measurement (1347.65us) within 5% from one wire-rate input."""
    s = wb.make_surrogate_from_wire(
        b_link_bytes_per_us=25000, num_lanes=18, algo="ring")
    pred = s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0)
    assert abs(pred - 1347.65) / 1347.65 <= 0.05
