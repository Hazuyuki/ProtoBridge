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
    assert abs(s.tree_bw_per_level - 270653) / 270653 <= 0.025
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
                     collective="allreduce") == pytest.approx(3015.56, rel=1e-3)
    assert s.predict(D, 8, "nvls", credits=128, ber=0,
                     collective="allgather") == pytest.approx(1018.01, rel=1e-3)


def test_fullmesh_steps_not_ring_fallthrough():
    """Full-mesh is 2 phases (AR: reduce-scatter + all-gather) / 1 phase (AG),
    NOT the ring's 2*(N-1) sequential steps -- the N-1 per-peer sends are
    concurrent on dedicated bundles. Without these branches, algo='fullmesh'
    silently fell through to the ring step count (14 at N=8)."""
    s = wb.make_h200_fullmesh_theory()
    assert s._steps(8, "fullmesh", "allreduce") == 2
    assert s._steps(8, "fullmesh", "allgather") == 1
    assert s._steps(8, "fullmesh", "allreduce") != 2 * (8 - 1)


def test_fullmesh_per_step_is_chunk_over_n():
    """Each phase ships the D/N chunk on each peer's dedicated bundle (the N-1
    sends are concurrent, so the per-step serialized volume is D/N, not the
    ring's D*(N-1)/N)."""
    s = wb.make_h200_fullmesh_theory()
    D = 256 * 1024 * 1024
    assert s._per_step(D, 8, "fullmesh", 2, "allreduce") == pytest.approx(D / 8)


def test_fullmesh_credit_pressure_is_one():
    """Full-mesh is single-hop per-peer on a dedicated bundle -- no pipelined
    credit pressure (like nvls), so Phi_credit = 1."""
    s = wb.make_h200_fullmesh_theory()
    D = 256 * 1024 * 1024
    assert s._credit_pressure(D, 8, "fullmesh", 128, "allreduce") == 1.0


def test_fullmesh_reproduces_ns3_ground_truth():
    """Surrogate fullmesh reproduces the ns-3 FullMeshAllReduce/AllGather
    simTimeUs (the calibration ground truth) within 2% on the SIMPLE regime
    (256 MiB / 8 GPUs: ns-3 AR=165.0 us, AG=90.0 us). fullmesh_bw is back-fit
    to this ns-3 sweep (configs/calibration/fullmesh_ns3_sweep.py)."""
    s = wb.make_h200_fullmesh_theory()
    D = 256 * 1024 * 1024
    ar = s.predict(D, 8, "fullmesh", credits=128, ber=0, collective="allreduce")
    ag = s.predict(D, 8, "fullmesh", credits=128, ber=0, collective="allgather")
    assert ar == pytest.approx(165.0, rel=2e-2)   # ns-3 GT 165.0
    assert ag == pytest.approx(90.0, rel=2e-2)    # ns-3 GT 90.0


def test_fullmesh_ar_is_two_ag_serial_parts():
    """AR = 2 phases, AG = 1 phase, so AR = 2*AG - startup (the serial+fixed
    work doubles, the startup is paid once). Exact structural invariant of the
    2-phase-vs-1-phase full-mesh model (Phi_credit=Phi_fec=Phi_share=1)."""
    s = wb.make_h200_fullmesh_theory()
    D = 256 * 1024 * 1024
    ar = s.predict(D, 8, "fullmesh", credits=128, ber=0, collective="allreduce")
    ag = s.predict(D, 8, "fullmesh", credits=128, ber=0, collective="allgather")
    assert ar == pytest.approx(2 * ag - s.S_startup, rel=1e-9)


def test_fullmesh_faster_than_ring_at_large_size():
    """Full-mesh uses all N-1 dedicated bundles concurrently, so at large
    messages it is far faster than the ring (which pipelines 2*(N-1) steps).
    ns-3: fullmesh AR 256 MiB / 8 = 165 us vs ring 1348 us (~8x)."""
    s = wb.make_h200_fullmesh_theory()
    D = 256 * 1024 * 1024
    fm = s.predict(D, 8, "fullmesh", credits=128, ber=0, collective="allreduce")
    ring = wb.make_h200_ring_theory().predict(
        D, 8, "ring", credits=128, ber=0, collective="allreduce")
    assert fm < ring
    assert ring / fm > 5.0   # ~8x at this point


def test_fullmesh_wire_factory_derives_full_bundle():
    """The wire-derived entry (make_surrogate_from_wire, eta_fullmesh=1.0)
    gives fullmesh_bw = eta * B_agg = 1.0 * (25000 * 18) = 450000 -- the full
    dedicated bundle, the single-parameter entry for non-H200 platforms."""
    s = wb.make_surrogate_from_wire(25000, 18, "fullmesh")
    assert s.fullmesh_bw == pytest.approx(450000)
    # and it reproduces the ns-3 GT just like the H200 preset
    D = 256 * 1024 * 1024
    assert s.predict(D, 8, "fullmesh", credits=128, ber=0,
                     collective="allreduce") == pytest.approx(165.0, rel=2e-2)


def test_tree_trunk_load_alpha():
    """alpha(N) = busiest GPU<->NVSwitch trunk load in D units for the
    heap-shaped binary tree: 1 (single edge), 2 (root at N<=4), 3 (interior
    node: 2D child + 1D parent traffic at N>=5)."""
    f = wb.TheoryDerivedSurrogate._tree_trunk_load
    assert f(2) == 1.0
    assert f(3) == 2.0
    assert f(4) == 2.0
    assert f(5) == 3.0
    assert f(8) == 3.0
    assert f(16) == 3.0


def test_tree_reproduces_ns3_ground_truth():
    """The recalibrated tree preset reproduces the post-volume-fix ns-3
    TreeAllReduce simTimeUs (the calibration ground truth,
    configs/calibration/tree_ns3_sweep.py) within 12% at 256 MiB (fit MAPE
    7.6% over all 27 points, 5.0% on the SIMPLE regime). startup_us=46 is the
    profile's SIMPLE startup (the model takes startup by D regime, like the
    sweep's back-fit)."""
    s = wb.make_h200_tree_theory()
    D = 256 * 1024 * 1024
    for n, gt in ((4, 1843.5), (8, 2833.5), (16, 2915.3)):
        pred = s.predict(D, n, "tree", credits=128, ber=0, startup_us=46.0,
                         collective="allreduce")
        assert abs(pred - gt) / gt <= 0.12, (n, pred, gt)


def test_tree_latency_not_decreasing_in_n():
    """Shape regression for the recalibration: the OLD serial term
    D(N-1)/N/(log2N*tree_bw) DECREASED with N; ns-3 measures a
    saturating-INCREASING trunk cost (alpha = 2.5 / 3.9 / 4.0 at N=4/8/16).
    The alpha(N) model must preserve that ordering."""
    s = wb.make_h200_tree_theory()
    D = 256 * 1024 * 1024
    t4, t8, t16 = (s.predict(D, n, "tree", credits=128, ber=0,
                             startup_us=46.0, collective="allreduce")
                   for n in (4, 8, 16))
    assert t4 < t8 <= t16


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


# ---- scale-out (Ethernet RoCEv2) + hierarchical (Su -> So) ----

def test_wire_efficiency_default_is_unity_byte_identical():
    """NVLink wire_efficiency defaults to 1.0 (no L3 header); applying it is a
    no-op, so make_h200_ring_theory is byte-identical to the pre-So model."""
    s = wb.make_h200_ring_theory()
    assert s.wire_efficiency == 1.0
    D = 1 << 22
    assert s.predict(D, 8, "ring", credits=32, collective="allreduce") == \
        s.predict(D, 8, "ring", credits=32, collective="allreduce",
                  topology=None)


def test_wire_efficiency_raises_latency():
    """A lower wire efficiency (more header overhead) must raise latency."""
    s = wb.make_h200_ring_theory(wire_efficiency=0.95)
    base = wb.make_h200_ring_theory()
    D = 1 << 24
    assert s.predict(D, 8, "ring", credits=128, ber=0,
                     collective="allreduce") > \
        base.predict(D, 8, "ring", credits=128, ber=0, collective="allreduce")


def test_phase_default_full_byte_identical():
    """phase='full' (default two-pass) is byte-identical to omitting phase."""
    s = wb.make_h200_ring_theory()
    D = 1 << 22
    assert s.predict(D, 8, "ring", credits=32, collective="allreduce") == \
        s.predict(D, 8, "ring", credits=32, collective="allreduce",
                  phase="full")


def test_phase_rs_ag_cheaper_than_full():
    """A single pass (rs/ag) has fewer steps so less fixed latency than the
    two-pass allreduce; the serial volume (steps x per_step) is invariant."""
    s = wb.make_h200_ring_theory()
    D = 1 << 22
    full = s.predict(D, 8, "ring", credits=32, collective="allreduce")
    rs = s.predict(D, 8, "ring", credits=32, collective="allreduce", phase="rs")
    ag = s.predict(D, 8, "ring", credits=32, collective="allreduce", phase="ag")
    assert rs < full
    assert ag < full
    assert abs(rs - ag) < 1e-9          # rs and ag are symmetric single passes


def test_roce_theory_defaults():
    """So RoCEv2 profile: 400GbE-derived ring bw, 40 us startup, 0.95 wire eff,
    8 NICs/node, ~1 us/hop, 1.5 us switch pipeline."""
    s = wb.make_h200_roce_theory()
    assert s.ring_bw == 160000               # 0.4 * 8 NICs * 50000 B/us
    assert s.S_startup == 40.0
    assert s.wire_efficiency == 0.95
    assert s.num_lanes == 8
    assert s.lat == 1.0
    assert s.T_min_switch == 1.5


def test_ethernet_theory_defaults():
    """So plain-Ethernet profile: same 400GbE ring bw as RoCE (160000), 40 us
    startup, wire_eff 1.0 (ns-3 EthernetProtocolModel locks WireEfficiency=1.0
    -- plain L2 data carries no RoCE IP/UDP/IB headers), 8 NICs/node, and a
    store-and-forward switch pipeline (~3 us/hop, 3 us switch) vs RoCE's
    cut-through 1.0/1.5."""
    s = wb.make_h200_ethernet_theory()
    assert s.ring_bw == 160000               # same 400GbE wire as RoCE
    assert s.S_startup == 40.0               # same NCCL inter-node startup
    assert s.wire_efficiency == 1.0          # ns-3 Ethernet locks eff=1.0
    assert s.num_lanes == 8
    assert s.lat == 3.0                       # store-and-forward (vs RoCE 1.0)
    assert s.T_min_switch == 3.0             # store-and-forward (vs RoCE 1.5)


def test_ethernet_slower_than_roce_small_faster_large():
    """At small messages the store-and-forward switch pipeline dominates, so
    plain Ethernet is slower than RoCE (cut-through). At large messages the
    serial term dominates and Ethernet is at least as fast (wire_eff 1.0 vs
    RoCE 0.95 -- no RoCE header overhead). Both share ring_bw/credits, so the
    relative ordering is set by latency (small) vs serial (large)."""
    eth = wb.make_h200_ethernet_theory()
    roce = wb.make_h200_roce_theory()
    small = 1 << 14   # 16 KiB -- latency-dominated
    big = 1 << 28     # 256 MiB -- serial-dominated
    e_small = eth.predict(small, 2, "ring", credits=128, ber=0,
                          collective="sendrecv")
    r_small = roce.predict(small, 2, "ring", credits=128, ber=0,
                           collective="sendrecv")
    assert e_small > r_small, \
        f"small-msg Ethernet {e_small} not > RoCE {r_small}"
    e_big = eth.predict(big, 2, "ring", credits=128, ber=0,
                         collective="sendrecv")
    r_big = roce.predict(big, 2, "ring", credits=128, ber=0,
                         collective="sendrecv")
    assert e_big <= r_big, \
        f"large-msg Ethernet {e_big} not <= RoCE {r_big}"


def test_hierarchical_with_ethernet_so():
    """HierarchicalSurrogate composes with a plain-Ethernet So profile via the
    so_factory seam: the Su (NVLink ring) stage is unchanged, the So stage uses
    make_h200_ethernet_theory (store-and-forward, wire_eff 1.0). The 2-node
    hierarchical allreduce exceeds the Su-only (1-node) allreduce (the So
    phase adds time), and the wired So profile is verifiably the Ethernet
    one (lat 3.0, wire_eff 1.0), not the RoCE default."""
    eth_h = wb.make_h200_hierarchical_theory(
        gpus_per_node=8, num_nodes=2, so_factory=wb.make_h200_ethernet_theory)
    roce_h = wb.make_h200_hierarchical_theory(gpus_per_node=8, num_nodes=2)
    su_only = wb.make_h200_ring_theory()
    D = 1 << 24   # 16 MiB
    t_eth = eth_h.predict(D, "ring", credits=128, collective="allreduce")
    t_su = su_only.predict(D, 8, "ring", credits=128, ber=0,
                           collective="allreduce")
    # the So stage adds time -> hierarchical > Su-only
    assert t_eth > t_su, f"Ethernet hierarchical {t_eth} not > Su-only {t_su}"
    # the So profile really IS the Ethernet one (store-and-forward lat 3.0)
    assert eth_h.so.lat == 3.0
    assert eth_h.so.wire_efficiency == 1.0
    assert roce_h.so.lat == 1.0           # RoCE default unchanged by the seam
    # the two So profiles give different hierarchical latencies (the seam
    # actually swaps the So stage, not a no-op)
    t_roce = roce_h.predict(D, "ring", credits=128, collective="allreduce")
    assert t_eth != t_roce


def test_connectx7_p2p_perftest_theory_defaults():
    """ConnectX-7 P2P perftest profile: 192 Gbps single-write rate, 1.45 us
    fixed latency (1.05 doorbell+completion + 0.4 prop), single QP, wire_eff
    1.0 (the 192 Gbps is already an effective rate)."""
    s = wb.make_connectx7_p2p_perftest_theory()
    assert s.ring_bw == 24000               # 192 Gbps single-write effective rate
    assert s.S_startup == 1.05             # doorbell + write completion
    assert s.lat == 0.4                    # same-switch propagation
    assert abs(s.S_startup + s.lat - 1.45) < 1e-9  # fixed overhead 1.45 us
    assert s.wire_efficiency == 1.0
    assert s.num_lanes == 1               # single QP / single flow


def test_connectx7_p2p_perftest_fits_ib_write_lat():
    """predict(sendrecv) reproduces the ib_write_lat single-write latency curve
    (ConnectX-7 400G, same switch) within 5% MAPE. The single-write curve is
    sublinear in NIC processing, so the linear surrogate fits the 2B/64B/64KB
    endpoints closely and the 1KB transition within ~8% (overall ~2.6% MAPE).
    """
    s = wb.make_connectx7_p2p_perftest_theory()
    # ib_write_lat t_avg (us) from configs/calibration/roce_p2p_calib.py
    lat = {2: 1.45, 64: 1.48, 1024: 1.62, 65536: 4.21}
    errs = []
    for D, meas in lat.items():
        pred = s.predict(D, 2, "ring", credits=128, ber=0, collective="sendrecv")
        errs.append(abs(pred - meas) / meas)
    mape = sum(errs) / len(errs) * 100.0
    assert mape < 5.0, f"ib_write_lat MAPE {mape:.2f}% exceeds 5%"
    # large-write endpoint (64 KB) fits within 2% (the sublinear regime starts
    # dominating only at the 1 KB transition)
    big = s.predict(65536, 2, "ring", credits=128, ber=0, collective="sendrecv")
    assert abs(big - 4.21) / 4.21 < 0.02


def test_so_topology_families_exist():
    """so_fullmesh / so_spineleaf / so_fattree build Topology objects; the
    non-blocking fullmesh caps no more than the oversubscribed spine-leaf."""
    fm = wb.make_topology_from_family("so_fullmesh", 8, links_per_gpu=8,
                                      per_link_gbps=50.0)
    sl = wb.make_topology_from_family("so_spineleaf", 8, links_per_gpu=8,
                                      per_link_gbps=50.0)
    ft = wb.make_topology_from_family("so_fattree", 8, links_per_gpu=8,
                                      per_link_gbps=50.0)
    assert fm.hop_count == 1.0
    assert sl.hop_count == 2.0
    assert ft.hop_count == 3.0
    # fullmesh = full bisection (non-blocking); spine-leaf/fattree = 8:1 cut.
    assert fm.bisection_bw_bytes_per_us >= sl.bisection_bw_bytes_per_us
    assert sl.bisection_bw_bytes_per_us == ft.bisection_bw_bytes_per_us


def test_hierarchical_collective_required():
    """collective is required keyword-only on the hierarchical predict too."""
    h = wb.make_h200_hierarchical_theory(gpus_per_node=8, num_nodes=2)
    with pytest.raises(TypeError):
        h.predict(1 << 22, "ring", 32)          # no collective=


def test_hierarchical_collective_is_keyword_only():
    """collective must be passed by keyword; a positional 4th arg is rejected
    (matches the base-class `*, collective` contract)."""
    h = wb.make_h200_hierarchical_theory(gpus_per_node=8, num_nodes=2)
    with pytest.raises(TypeError):
        h.predict(1 << 22, "ring", 32, "allreduce")  # positional collective


def test_hierarchical_collective_validated():
    """An invalid collective raises ValueError, not a silent fallback."""
    h = wb.make_h200_hierarchical_theory()
    with pytest.raises(ValueError):
        h.predict(1 << 22, "ring", 32, collective="bogus")


def test_hierarchical_allreduce_exceeds_su_only():
    """The So inter-node phase adds time: hierarchical > Su-only allreduce."""
    D = 1 << 24
    su_only = wb.make_h200_ring_theory().predict(
        D, 8, "ring", credits=128, collective="allreduce")
    hier = wb.make_h200_hierarchical_theory(
        gpus_per_node=8, num_nodes=2).predict(
        D, "ring", credits=128, collective="allreduce")
    assert hier > su_only


def test_hierarchical_monotonic_in_nodes():
    """More nodes -> more So steps (toward the D/So_bw asymptote), so the
    hierarchical latency grows with node count."""
    D = 1 << 24
    lats = [wb.make_h200_hierarchical_theory(
        gpus_per_node=8, num_nodes=n).predict(
        D, "ring", credits=128, collective="allreduce")
            for n in (2, 4, 8)]
    assert lats[0] < lats[1] < lats[2]


def test_hierarchical_topology_lower_bound_ordering():
    """Non-blocking so_fullmesh is the lower bound; oversubscribed / multi-hop
    fabrics add latency, so fullmesh <= spine-leaf <= fat-tree."""
    D = 1 << 24
    lats = {fam: wb.make_h200_hierarchical_theory(
        gpus_per_node=8, num_nodes=4, so_topology_family=fam).predict(
        D, "ring", credits=128, collective="allreduce")
        for fam in ("so_fullmesh", "so_spineleaf", "so_fattree")}
    assert lats["so_fullmesh"] <= lats["so_spineleaf"] <= lats["so_fattree"]


def test_hierarchical_decomposition_additive():
    """The 4-phase allreduce equals the sum of its single-pass sub-phases."""
    D = 1 << 24
    h = wb.make_h200_hierarchical_theory(gpus_per_node=8, num_nodes=2)
    su, so = h.su, h.so
    topo = h.so_topology
    n_su, n_so = h.n_su, h.n_so
    # Su phases move the full D; inter-node So phases move the per-node
    # reduced chunk D/n_su (matching the 4-phase allreduce decomposition).
    so_chunk = D // n_su
    phases = (
        su.predict(D, n_su, "ring", 128, collective="allreduce", phase="rs")
        + so.predict(so_chunk, n_so, "ring", 128, collective="allreduce",
                     phase="rs", topology=topo)
        + so.predict(so_chunk, n_so, "ring", 128, collective="allreduce",
                     phase="ag", topology=topo)
        + su.predict(D, n_su, "ring", 128, collective="allreduce", phase="ag"))
    assert abs(h.predict(D, "ring", 128, collective="allreduce") - phases) < 1e-6


def test_hierarchical_allgather_cheaper_than_allreduce():
    """Allgather is 3-phase (one fewer So pass) so it is cheaper than the
    4-phase allreduce on the same fabric."""
    D = 1 << 24
    h = wb.make_h200_hierarchical_theory(gpus_per_node=8, num_nodes=4)
    ag = h.predict(D, "ring", 128, collective="allgather")
    ar = h.predict(D, "ring", 128, collective="allreduce")
    assert ag < ar


def test_hierarchical_sendrecv_is_so_only():
    """A single inter-node P2P transfer has no Su stage: it equals the So
    surrogate's sendrecv on n_so nodes under the So topology."""
    D = 1 << 22
    h = wb.make_h200_hierarchical_theory(gpus_per_node=8, num_nodes=2)
    direct = h.so.predict(D, h.n_so, "ring", 32, collective="sendrecv",
                          topology=h.so_topology)
    assert abs(h.predict(D, "ring", 32, collective="sendrecv") - direct) < 1e-6
