"""Test the theory-derived (whitebox) surrogate against the H200 ring4 baseline.

The theory model is a first-principles physical derivation (startup +
serialization + credit round-trip + FEC/retry terms), NOT a regression fit.
It is therefore a lower bound on the ns-3 measurement (it omits firmware
overheads, arbiter jitter, VOQ drain) — so we assert a generous physical
plausibility band (within 2x), not the <1% MAPE that the calibrated
analytical surrogate achieves.
"""
import json
import math
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
    lat = s.predict(1 << 20, 4, "ring", credits=32, ber=0, collective="allreduce")
    assert isinstance(lat, float)
    assert lat > 0.0
    assert lat == lat  # not NaN


def test_latency_monotonic_in_size():
    """Larger message -> larger latency (the model must capture serialization)."""
    s = wb.make_h200_ring_theory()
    small = s.predict(1 << 16, 4, "ring", credits=32, ber=0, collective="allreduce")
    large = s.predict(1 << 26, 4, "ring", credits=32, ber=0, collective="allreduce")
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
    pred = s.predict(pt["dataSize"], 4, "ring", credits=32, ber=0, collective="allreduce")
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
    lat_default = s_default.predict(1 << 22, 8, "ring", credits=32, ber=0, collective="allreduce")
    lat_fast = s_fast.predict(1 << 22, 8, "ring", credits=32, ber=0, collective="allreduce")
    assert lat_fast < lat_default


def test_startup_override_is_additive():
    """startup_us is an additive fixed term: +35us startup -> +~35us latency."""
    base = wb.make_h200_ring_theory().predict(1 << 20, 4, "ring", credits=32, ber=0, collective="allreduce")
    raised = wb.make_h200_ring_theory(startup_us=50.0).predict(
        1 << 20, 4, "ring", credits=32, ber=0, collective="allreduce")
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
    pred = s.predict(256 * 1024 * 1024, 8, "ring", credits=128, ber=0, collective="allreduce")
    assert abs(pred - 1347.65) / 1347.65 <= 0.05


# ---- topology kwarg (bisection cap on bandwidth + hop latency) ----

def test_topology_none_byte_identical():
    """Omitting topology and passing topology=None give the same prediction."""
    s = wb.make_h200_ring_theory()
    a = s.predict(1 << 22, 8, "ring", credits=32, ber=0, collective="allreduce")
    b = s.predict(1 << 22, 8, "ring", credits=32, ber=0, collective="allreduce", topology=None)
    assert a == b


def test_topology_default_reproduces_ideal():
    """Topology() with defaults (inf bisection, hop=1) == ideal fabric."""
    s = wb.make_h200_ring_theory()
    a = s.predict(1 << 22, 8, "ring", credits=32, ber=0, collective="allreduce")
    b = s.predict(1 << 22, 8, "ring", credits=32, ber=0, collective="allreduce", topology=wb.Topology())
    assert a == b


def test_topology_bisection_caps_bandwidth():
    """A tight fabric bisection caps the schedule rate -> higher latency."""
    s = wb.make_h200_ring_theory()
    ideal = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce")
    # 8-GPU ring topology: bisection = 2 links x 25 GB/s = 50 GB/s
    t = wb.make_topology(bisection_gbps=50, hop_count=1)
    capped = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce", topology=t)
    assert capped > ideal


def test_topology_hop_count_adds_latency():
    """A multi-hop fabric (hop_count>1) adds per-step link propagation."""
    s = wb.make_h200_ring_theory()
    ideal = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce")
    t = wb.make_topology(bisection_gbps=None, hop_count=3)
    multi = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce", topology=t)
    # ring: 2(N-1)=14 steps, each adds (hop-1)*link_lat = (3-1)*0.4us -> +11.2us
    assert abs((multi - ideal) - 14 * (3 - 1) * 0.4) < 1e-6


def test_make_topology_unit_conversion():
    """make_topology takes bisection in GB/s and stores bytes/us (x1000)."""
    assert wb.make_topology(bisection_gbps=50).bisection_bw_bytes_per_us == 50000.0
    assert wb.make_topology(bisection_gbps=50).hop_count == 1.0
    assert wb.make_topology(bisection_gbps=None).bisection_bw_bytes_per_us == float('inf')


# ---- make_topology_from_family (name + params auto-compute) ----

def test_make_topology_from_family_switched_is_ideal():
    """A non-blocking NVSwitch fabric needs no per_link and equals the ideal."""
    t = wb.make_topology_from_family("switched", N=8)
    assert t.bisection_bw_bytes_per_us == float('inf')
    assert t.hop_count == 1.0
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    ideal = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce")
    named = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce", topology=t)
    assert named == ideal  # byte-identical to topology=None


def test_make_topology_from_family_ring_matches_hand_computed():
    """ring(N=8, per_link=25) -> bisection 2*25=50 GB/s, hop 1 (the doc example)."""
    t = wb.make_topology_from_family("ring", N=8, per_link_gbps=25)
    assert t.bisection_bw_bytes_per_us == 50000.0       # 50 GB/s x1000
    assert t.hop_count == 1.0
    assert t == wb.make_topology(bisection_gbps=50, hop_count=1)


def test_make_topology_from_family_ring_slower_than_ideal():
    """A ring topology's tight bisection caps the rate -> higher latency."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    ideal = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce")
    t = wb.make_topology_from_family("ring", N=8, per_link_gbps=25)
    capped = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce", topology=t)
    assert capped > ideal


def test_make_topology_from_family_fullmesh_no_cap():
    """fullmesh(N=8) bisection = 8*7*25/2=700 GB/s >> ring_bw -> uncapped."""
    t = wb.make_topology_from_family("fullmesh", N=8, per_link_gbps=25)
    assert t.bisection_bw_bytes_per_us == 700000.0
    assert t.hop_count == 1.0
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    ideal = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce")
    named = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce", topology=t)
    assert named == ideal


def test_make_topology_from_family_torus_bisection():
    """3d_torus(N=64, dims=(4,4,4)) bisection = (64//4)*25/2 = 200 GB/s."""
    t = wb.make_topology_from_family(
        "3d_torus", N=64, per_link_gbps=25, dims=(4, 4, 4))
    assert t.bisection_bw_bytes_per_us == 200000.0
    assert t.hop_count == 1.0


def test_make_topology_from_family_leaftier_hop_and_bisection():
    """leafspine(N=32, num_leaf=8): hop = (23*1+8*2)/31 = 39/31; bisection = 32*8*25/8."""
    t = wb.make_topology_from_family(
        "leafspine", N=32, per_link_gbps=25, num_leaf=8)
    assert abs(t.hop_count - 39.0 / 31.0) < 1e-9
    assert t.bisection_bw_bytes_per_us == 800000.0          # 800 GB/s x1000


def test_make_topology_from_family_ring_requires_per_link():
    """A bisection-bound family raises without per_link_gbps."""
    with pytest.raises(ValueError):
        wb.make_topology_from_family("ring", N=8)


def test_make_topology_from_family_unknown_raises():
    """An unknown family name raises ValueError."""
    with pytest.raises(ValueError):
        wb.make_topology_from_family("bogus", N=8, per_link_gbps=25)


# ---- collective kwarg (REQUIRED: collective + algorithm select the step model) ----

def test_collective_is_required():
    """collective is a mandatory keyword argument -- omitting it raises
    TypeError. There is NO inference from algo; the caller must name it."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    with pytest.raises(TypeError):
        s.predict(1 << 24, 8, "ring", credits=128)


def test_collective_invalid_raises():
    """An unknown collective name raises ValueError (validated, not inferred)."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    with pytest.raises(ValueError):
        s.predict(1 << 24, 8, "ring", credits=128, collective="bogus")


def test_collective_explicit_reproduces_calibrated():
    """An explicit collective reproduces the calibrated wire-derived latency
    for every schedule (ring/tree AllReduce, nvls AllGather). Since collective
    is no longer inferred, these values are fixed by the caller's choice."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    D = 256 * 1024 * 1024
    assert s.predict(D, 8, "ring", credits=128, ber=0,
                     collective="allreduce") == pytest.approx(1325.49, rel=1e-3)
    assert s.predict(D, 8, "tree", credits=128, ber=0,
                     collective="allreduce") == pytest.approx(3004.22, rel=1e-3)
    assert s.predict(D, 8, "nvls", credits=128, ber=0,
                     collective="allgather") == pytest.approx(1018.01, rel=1e-3)


def test_collective_explicit_retx_path():
    """The ber>0 link-retry path (GBN cascade) runs under an explicit
    collective without crashing and adds retransmission overhead over ber=0."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    D = 256 * 1024 * 1024
    clean = s.predict(D, 8, "ring", credits=128, ber=0, collective="allreduce")
    noisy = s.predict(D, 8, "ring", credits=128, ber=1e-8,
                      llr_enabled=True, collective="allreduce")
    assert math.isfinite(noisy)
    assert noisy > clean


# ---- alltoall (collective='alltoall') ----

def test_alltoall_ring_steps_is_n_minus_1():
    """AlltoAll ring runs N-1 permutation phases (one per partner)."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    assert s._steps(8, "ring", "alltoall") == 7
    assert s._steps(4, "ring", "alltoall") == 3


def test_alltoall_tree_steps_is_ceil_log2():
    """AlltoAll tree uses ceil(log2 N) recursive-doubling rounds."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    assert s._steps(8, "tree", "alltoall") == 3      # ceil(log2 8) = 3
    assert s._steps(4, "tree", "alltoall") == 2      # ceil(log2 4) = 2


def test_alltoall_collnetdirect_steps_is_n_minus_1():
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    assert s._steps(8, "collnetdirect", "alltoall") == 7


def test_alltoall_per_step_and_volume():
    """AlltoAll moves D/N per phase, D*(N-1)/N per GPU in total."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    D, N = 1 << 24, 8
    steps = s._steps(N, "ring", "alltoall")
    per_step = s._per_step(D, N, "ring", steps, "alltoall")
    assert abs(per_step - D / N) < 1e-6                  # D/N per partner phase
    assert abs(per_step * steps - D * (N - 1) / N) < 1e-6  # full per-GPU volume


def test_alltoall_same_volume_as_allreduce_ideal():
    """On an ideal fabric alltoall and allreduce move the same per-GPU volume
    D*(N-1)/N over the ring, so their latencies are close (the difference
    appears under bisection contention, see the next test)."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    a2a = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="alltoall")
    ar = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="allreduce")
    assert abs(a2a - ar) / ar < 0.05


def test_alltoall_bisection_cap_raises_latency():
    """AlltoAll has tight bisection pressure (every GPU talks to every other);
    a fabric bisection cap captures that contention -> higher latency."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    ideal = s.predict(1 << 24, 8, "ring", credits=128, ber=0, collective="alltoall")
    t = wb.make_topology(bisection_gbps=50, hop_count=1)   # 8-GPU ring bisection
    capped = s.predict(1 << 24, 8, "ring", credits=128, ber=0,
                       collective="alltoall", topology=t)
    assert capped > ideal


# ---- send-recv (collective='sendrecv') ----

def test_sendrecv_steps_and_per_step():
    """Send-Recv is a single hop: steps=1, the full payload moves per step."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    D, N = 1 << 24, 8
    assert s._steps(N, "ring", "sendrecv") == 1
    assert s._per_step(D, N, "ring", 1, "sendrecv") == D


def test_sendrecv_no_credit_pressure():
    """A single hop has no pipeline to keep busy: Phi_credit=1, BDP=inf."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    assert s._credit_pressure(1 << 24, 8, "ring", 128, "sendrecv") == 1.0
    assert s._bdp(8, "ring", "sendrecv") == float('inf')


def test_sendrecv_latency_decomposition():
    """At ber=0 send-recv latency is just startup + D/ring_bw + link latency."""
    s = wb.make_surrogate_from_wire(25000, 18, "ring")
    D = 256 * 1024 * 1024
    pred = s.predict(D, 8, "ring", credits=128, ber=0, collective="sendrecv")
    expected = s.S_startup + D / s.ring_bw + s.lat
    assert abs(pred - expected) < 1e-6


# ---- alltoall / send-recv factories ----

def test_make_h200_alltoall_theory_defaults():
    """H200 AlltoAll profile: SIMPLE 46 us startup, 18 NVLink lanes."""
    s = wb.make_h200_alltoall_theory()
    assert s.S_startup == 46.0
    assert s.num_lanes == 18


def test_make_h200_sendrecv_theory_defaults():
    """H200 Send-Recv profile: LL 15 us startup, 18 NVLink lanes."""
    s = wb.make_h200_sendrecv_theory()
    assert s.S_startup == 15.0
    assert s.num_lanes == 18
