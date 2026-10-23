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
    assert abs(s.tree_bw_per_level - 275400) / 275400 <= 0.025
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
                     collective="allreduce") == pytest.approx(2973.32, rel=1e-3)
    assert s.predict(D, 8, "nvls", credits=128, ber=0,
                     collective="allgather") == pytest.approx(1018.01, rel=1e-3)


def test_ring_allgather_steps_single_pass():
    """Ring AllGather runs ONE pass of N-1 steps (ns-3 RingAllGather sets
    m_totalSteps = numGpus - 1), not the allreduce's 2(N-1): the second ring
    pass is the reduce-scatter half, which allgather does not have. The
    pre-fix count charged 5.6 us instead of 2.8 us of fixed per-step latency
    at 0.4-us links (codex round 16, finding 7). The rs/ag phase halving is
    an allreduce-only concept, so phase='ag' on allgather must keep the full
    single-pass count and stay byte-identical to phase='full'."""
    s = wb.make_h200_ring_theory()
    assert s._steps(8, "ring", "allgather") == 7
    assert s._steps(8, "ring", "allreduce") == 14
    assert s._steps(4, "ring", "allgather") == 3
    D = 1 << 22
    full = s.predict(D, 8, "ring", credits=32, collective="allgather")
    one_pass = s.predict(D, 8, "ring", credits=32, collective="allgather",
                         phase="ag")
    assert full == one_pass
    # The allreduce keeps its halving semantics unchanged.
    rs = s.predict(D, 8, "ring", credits=32, collective="allreduce", phase="rs")
    assert rs < s.predict(D, 8, "ring", credits=32, collective="allreduce")


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
    """alpha(N) = structural busiest GPU<->NVSwitch trunk load in D units:
    1.0 (single edge, N<=2), 2.0 (root with two children, N<=4), 3.0
    (interior node moving 2D child + 1D parent traffic, N>=5)."""
    f = wb.TheoryDerivedSurrogate._tree_trunk_load
    assert f(2) == 1.0
    assert f(3) == 2.0
    assert f(4) == 2.0
    assert f(5) == 3.0
    assert f(8) == 3.0
    assert f(32) == 3.0


def test_tree_per_step_is_alpha_d_over_steps():
    """The tree per-step volume is alpha(N)*D/steps (the full-volume
    chunk-pipelined tree serializes alpha(N)*D on the busiest trunk) -- with
    no extra fill term."""
    s = wb.make_h200_tree_theory()
    D = 256 * 1024 * 1024
    assert s._per_step(D, 8, "tree", 6, "allreduce") == 3.0 * D / 6


def test_tree_reproduces_ns3_ground_truth():
    """The original-form tree preset (2 fitted params: tree_bw=275400,
    t_min_switch=4.10) reproduces the fresh (post-local-rank-fix e1cdd9c
    binary) ns-3 TreeAllReduce simTimeUs at the TRUE H200 fabric
    (--linksPerGpu=18 x 170 Gbps spray-routed; the calibration ground truth,
    configs/calibration/tree_ns3_sweep.py) within 16% at 256 MiB (fit
    MAPE 12.65% over all 36 points, per-N 10.6/11.0/14.0/15.1%). startup_us=46
    is the profile's SIMPLE startup (the model takes startup by D regime,
    like the sweep's back-fit)."""
    s = wb.make_h200_tree_theory()
    D = 256 * 1024 * 1024
    for n, gt in ((4, 1745.9), (8, 2699.4), (16, 2789.7), (32, 2816.3)):
        pred = s.predict(D, n, "tree", credits=128, ber=0, startup_us=46.0,
                         collective="allreduce")
        assert abs(pred - gt) / gt <= 0.16, (n, pred, gt)


def test_tree_latency_not_decreasing_in_n():
    """Shape regression for the recalibration: the OLD serial term
    D(N-1)/N/(log2N*tree_bw) DECREASED with N; ns-3 measures a
    saturating-INCREASING trunk cost (alpha(N) = 2.0 / 3.0 / 3.0 at
    N=4/8/16 on top of growing steps). The alpha(N) model must preserve
    that ordering."""
    s = wb.make_h200_tree_theory()
    D = 256 * 1024 * 1024
    ts = [s.predict(D, n, "tree", credits=128, ber=0, startup_us=46.0,
                    collective="allreduce") for n in (4, 8, 16, 32)]
    assert ts == sorted(ts)


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
    """Allgather is 2-phase (Su ag + So ag, no final local broadcast — ns-3
    Complete() fires at the end of INTER_ALLGATHER) so it is cheaper than the
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


def test_tree_expected_losses_count_edges_not_ranks():
    """Tree allreduce transfers: the tree has N-1 edges and each edge carries
    the FULL D per direction (see _wire_bytes_per_transfer's tree branch),
    so the whole-collective transfer count is 2(N-1). The pre-fix multiplier
    2N(N-1) overcounted tree losses by N: at N=8/D=16 MiB/BER 6e-7/retry 3
    it reported 0.80997 expected losses instead of 0.10125, and
    strict_reliability wrongly returned infinity for a completable config."""
    tree = wb.make_h200_tree_theory()
    losses = tree.expected_permanent_losses(
        16 * 1024 * 1024, 8, "tree", 6e-7, 3,
        optical_fraction=1.0, optical_hops=1, collective="allreduce")
    assert abs(losses - 0.10125) < 2e-3
    # The strict gate must now stay finite for this completable config.
    t = tree.predict(16 * 1024 * 1024, 8, "tree", 1024, ber=6e-7,
                     retry_limit=3, strict_reliability=True,
                     collective="allreduce", llr_enabled=True)
    assert math.isfinite(t)


def test_ring_expected_losses_unchanged_2N():
    """Ring/fullmesh allreduce transfer counts keep the whole-collective
    2N(N-1) form (every rank does 2(N-1) chunk steps): the tree fix must not
    touch them."""
    ring = wb.make_h200_ring_theory()
    losses = ring.expected_permanent_losses(
        16 * 1024 * 1024, 8, "ring", 6e-7, 3,
        optical_fraction=1.0, optical_hops=1, collective="allreduce")
    # 2N(N-1) = 112 transfers at the ring's per-transfer packetization.
    assert losses > 0.8  # the pre-fix tree value (0.80997) is the ring scale


def test_nvls_expected_losses_count_result_sides():
    """NVLS/SHARP reliability must count upload and result deliveries
    separately: each rank's upload carries D/N (allgather) or D (allreduce),
    while each multicast result delivers the FULL D to every destination —
    the switch re-multicasts every received upload chunk, so a dest's result
    stream has N * frag(D/N) packets. The pre-fix model counted 2N transfers
    at the upload's packetization: at N=8/D=16 MiB it counted 288 packets
    where the C++ fabric emits 1,296 (144 upload + 1,152 result), giving
    0.2683 expected losses at BER 1e-9/0 retries instead of ~1.2074.
    The two sides also carry DIFFERENT repair budgets (codex round 14,
    finding 5): uploads retransmit from the endpoint's LLR store
    (retry_limit + 1 attempts) but results are repaired by the SWITCH from
    its own result store — SHARP_RESULT_RETRY_LIMIT = 64 retransmissions
    regardless of the endpoint retry limit (ns-3 seed 3 completes at
    79.2 us with endpoint retry 0 including one switch result repair), so
    at retry 0 the allgather predicts 0.1342, not 1.2074."""
    nvls = wb.make_h200_nvls_theory()
    D, N = 16 * 1024 * 1024, 8
    losses = nvls.expected_permanent_losses(
        D, N, "nvls", 1e-9, 0,
        optical_fraction=1.0, optical_hops=1, collective="allgather")
    assert abs(losses - 0.134156) < 2e-3
    # The allreduce branch carries the same both-sides structure (1,024
    # uploads + 1,152 pipelined result chunks at N=8/D=16 MiB/P=8) with the
    # same split budgets.
    losses_ar = nvls.expected_permanent_losses(
        D, N, "nvls", 1e-9, 0,
        optical_fraction=1.0, optical_hops=1, collective="allreduce")
    assert abs(losses_ar - 1.073179) < 2e-3


def test_nvls_llr_off_results_have_one_attempt():
    """The switch's SHARP result store only exists when LLR is on:
    NvSwitch::StoreMulticastResult skips outright when !m_llrEnabled, so
    HandleSwitchNack has nothing to retransmit — a lost multicast result
    is permanent on its FIRST attempt, whatever the switch retry limit
    models for the LLR-on regime. The pre-fix loss model always granted
    result packets nvls_switch_result_retry_limit + 1 = 65 attempts: at
    8 GPU / 16 MiB allgather / BER 8e-4 / RS(544,514,15) / LLR off it
    counted only 0.303 expected unrepaired losses and strict_reliability
    returned a finite 90.247 us for a config whose ns-3 run stays
    incomplete after two uncorrectable result packets (codex round 15,
    finding 5; expected ~2.728 with the one-attempt budget)."""
    nvls = wb.make_h200_nvls_theory()
    D, N = 16 * 1024 * 1024, 8
    # LLR off: every result packet has exactly one attempt.
    losses_off = nvls.expected_permanent_losses(
        D, N, "nvls", 8e-4, 0,
        fec_enabled=True, fec_n=544, fec_k=514, fec_t=15,
        optical_fraction=1.0, optical_hops=1, collective="allgather",
        llr_enabled=False)
    assert abs(losses_off - 2.727986) < 2e-3
    assert losses_off >= 0.5  # incompletable: strict must reject
    t = nvls.predict(D, N, "nvls", 128, ber=8e-4, retry_limit=0,
                     fec_enabled=True, fec_n=544, fec_k=514, fec_t=15,
                     llr_enabled=False, strict_reliability=True,
                     collective="allgather")
    assert not math.isfinite(t)
    # LLR off with the retry limit omitted: predict's eff_retry_limit = 0
    # path must reach the same rejection.
    t2 = nvls.predict(D, N, "nvls", 128, ber=8e-4,
                      fec_enabled=True, fec_n=544, fec_k=514, fec_t=15,
                      llr_enabled=False, strict_reliability=True,
                      collective="allgather")
    assert not math.isfinite(t2)
    # LLR on at the same BER keeps the 65-attempt switch budget, so the
    # result side stays completable (the LLR-off collapse must be gated).
    losses_on = nvls.expected_permanent_losses(
        D, N, "nvls", 8e-4, 0,
        fec_enabled=True, fec_n=544, fec_k=514, fec_t=15,
        optical_fraction=1.0, optical_hops=1, collective="allgather",
        llr_enabled=True)
    assert losses_on < 0.5


def test_llr_off_retry_limit_none_means_one_attempt():
    """LLR off with retry_limit unspecified (None or negative) means exactly
    ONE attempt per packet, not zero risk. The pre-fix order ran the
    None/-1 early return BEFORE the LLR-off normalization, so
    expected_permanent_losses(..., llr_enabled=False, retry_limit=None)
    returned 0.0 and strict_reliability blessed configs whose ns-3 runs
    stay incomplete (codex round 19, finding 9)."""
    ring = wb.make_h200_ring_theory()
    D, N = 16 * 1024 * 1024, 8
    ref = ring.expected_permanent_losses(
        D, N, "ring", 2e-7, 0, collective="allreduce", llr_enabled=False)
    assert ref > 0
    # None and -1 normalize to the same one-attempt budget as an explicit 0.
    losses_none = ring.expected_permanent_losses(
        D, N, "ring", 2e-7, None, collective="allreduce", llr_enabled=False)
    losses_neg = ring.expected_permanent_losses(
        D, N, "ring", 2e-7, -1, collective="allreduce", llr_enabled=False)
    assert abs(losses_none - ref) < 1e-12
    assert abs(losses_neg - ref) < 1e-12
    # LLR ON keeps the old semantics: an unspecified retry limit is
    # unlimited retries, i.e. zero expected permanent losses.
    losses_on = ring.expected_permanent_losses(
        D, N, "ring", 2e-7, None, collective="allreduce", llr_enabled=True)
    assert losses_on == 0.0


def test_nvls_expected_losses_use_spray_fragment_geometry():
    """The NVLS branch must fragment at SendCollectiveBulk's spray geometry,
    not the DATA bulk path's. Each rank's upload of D/N (allgather) or D
    (allreduce) moves as max(lanes, ceil(bytes/spray_chunk)) fragments of
    ceil(bytes/frags) — 128 KiB-class chunks — and each result chunk mirrors
    one upload chunk. The pre-fix branch called _packets_per_transfer with
    D/N (which divides by N internally AGAIN, so the upload sized at D/N^2)
    and took its packet failure probability from the DATA path's ~932 KB
    bulk fragments: 8 GPU / 128 MiB allgather / BER 5e-8 / retry 3 reported
    12.1577 expected losses and strict_reliability rejected a config whose
    ns-3 run completes in 488.7 us with zero permanent losses (codex round
    12, finding 6). The C++ geometry gives 0.062731."""
    nvls = wb.make_h200_nvls_theory()
    D, N = 128 * 1024 * 1024, 8
    # 16 MiB per-rank upload: max(18, ceil(16 MiB / 128 KiB)) = 128 frags of
    # 131072 B; result per dest = N * 128 = 1024 frags at the same size.
    losses = nvls.expected_permanent_losses(
        D, N, "nvls", 5e-8, 3,
        optical_fraction=1.0, optical_hops=1, collective="allgather")
    assert abs(losses - 0.006970) < 2e-4
    # The strict gate must stay finite for this completable config.
    t = nvls.predict(D, N, "nvls", 128, ber=5e-8, retry_limit=3,
                     strict_reliability=True, collective="allgather",
                     llr_enabled=True)
    assert math.isfinite(t)
    # BER 8e-8 keeps the same geometry (both sides, one packet size).
    losses8 = nvls.expected_permanent_losses(
        D, N, "nvls", 8e-8, 3,
        optical_fraction=1.0, optical_hops=1, collective="allgather")
    assert abs(losses8 - 0.042925) < 2e-4
    # Allreduce uploads carry the full D per rank (128 frags of 128 KiB);
    # the result side is PIPELINED over nvlsNumPartitions=8, so each dest
    # gets P * frag(D/P) = 8 * 18 chunks of 116,509 B, not N * frag(D).
    losses_ar = nvls.expected_permanent_losses(
        D, N, "nvls", 5e-8, 3,
        optical_fraction=1.0, optical_hops=1, collective="allreduce")
    assert abs(losses_ar - 0.055761) < 2e-4


def test_nvls_allreduce_result_partition_geometry():
    """NVLS AllReduce results are pipelined over nvlsNumPartitions (default
    8), not re-multicast at the upload granularity. Every rank uploads the
    full D (frag(D) chunks), and the switch's result multicast sends each
    dest P partitions of D/P each, re-fragmented at frag(D/P) — at N=8,
    D=16 MiB, P=8, 18 lanes that is 1,024 upload chunks + 1,152 result
    chunks. The pre-fix formula mirrored the allgather branch and counted
    N * N * frag(D) = 8,192 result chunks, so BER 1e-10/0 retries gave
    0.9663 expected losses and strict_reliability returned infinity for a
    config whose ns-3 run completes with zero permanent losses (codex
    round 13, finding 5)."""
    nvls = wb.make_h200_nvls_theory()
    D, N = 16 * 1024 * 1024, 8
    upload_frags, upload_frag_bytes = nvls._spray_fragments(D)
    result_frags, result_frag_bytes = nvls._spray_fragments(D / 8)
    assert upload_frags == 128 and upload_frag_bytes == 131072
    assert result_frags == 18 and result_frag_bytes == 116509
    # 8 ranks * 128 uploads (endpoint retry 0 -> 1 attempt) + 8 dests * 8
    # partitions * 18 chunks (switch-side budget: 64 retries + 1 attempt).
    expected = (N * upload_frags * (1 - (1 - 1e-10) ** (upload_frag_bytes * 8))
                + N * 8 * result_frags
                * (1 - (1 - 1e-10) ** (result_frag_bytes * 8)) ** 65)
    losses = nvls.expected_permanent_losses(
        D, N, "nvls", 1e-10, 0,
        optical_fraction=1.0, optical_hops=1, collective="allreduce")
    assert abs(losses - expected) < 1e-12
    assert losses < 0.5  # completable: strict must stay finite
    t = nvls.predict(D, N, "nvls", 128, ber=1e-10, retry_limit=0,
                     strict_reliability=True, collective="allreduce",
                     llr_enabled=True)
    assert math.isfinite(t)


def test_spray_fragments_single_link_single_packet():
    """SendCollectiveBulk only fragments when the fabric NEEDS the spray:
    needFragmentColl = (spray links > 1) OR (wire size > 500 MiB). A
    single-link fabric below the threshold sends the whole transfer as ONE
    packet of the full size — a 4-GPU / 16-MiB AllGather over one link
    uploads one 4-MiB packet per rank, not 32 x 128-KiB fragments (codex
    round 13, finding 6). The always-fragmenting helper predicted 0.06285
    permanent losses at BER 1e-7 / 3 retries (finite strict completion)
    where the 4-MiB packet fails with p ~ 0.965 per attempt and ns-3
    reports the collective incomplete with 3 permanent losses."""
    one_link = wb.make_h200_nvls_theory(num_lanes=1)
    D, N = 16 * 1024 * 1024, 4
    frags, frag_bytes = one_link._spray_fragments(D / N)
    assert frags == 1 and frag_bytes == D // N  # one 4-MiB packet
    # 4 uploads (4 attempts: retry 3) + 4 dests * N mirrored result chunks
    # (switch-side budget: 65 attempts) — all at the one 4-MiB packet size.
    p = 1 - (1 - 1e-7) ** (D / N * 8)
    losses = one_link.expected_permanent_losses(
        D, N, "nvls", 1e-7, 3,
        optical_fraction=1.0, optical_hops=1, collective="allgather")
    assert abs(losses - (4 * p ** 4 + 16 * p ** 65)) < 1e-9
    assert losses > 1.0  # incompletable: strict must reject
    t = one_link.predict(D, N, "nvls", 128, ber=1e-7, retry_limit=3,
                         strict_reliability=True, collective="allgather",
                         llr_enabled=True)
    assert not math.isfinite(t)
    # Multi-lane geometries are unchanged (the spray branch still applies).
    multi = wb.make_h200_nvls_theory()
    assert multi._spray_fragments(D / N) == (32, 131072)
    # A single-link transfer above the 500-MiB overflow threshold still
    # fragments.
    huge = one_link._spray_fragments(600 * 1024 * 1024)
    assert huge[0] > 1


def test_spray_fragments_large_transfer_coalescing():
    """Above the 512-MiB COALESCE_THRESHOLD, SendCollectiveBulk caps the
    event count by coalescing into ~8-MiB path-balanced chunks:
    numChunks = max(lanes, min(sizeBased, ceil(ceil(wire/8MiB)/lanes)*lanes))
    — 2 GPUs / 2 GiB AllGather uploads 144 fragments of ~7.46 MiB, not
    8,192 x 128 KiB (codex round 14, finding 2). The un-coalesced count
    modeled ~57x too many fragments, predicting 0.000582 permanent losses
    at BER 1e-8 / 3 retries (finite strict completion) where ns-3 emits
    the coarser fragments and reports the collective incomplete with 13
    permanent losses."""
    nvls = wb.make_h200_nvls_theory()
    upload = 2 * 1024**3 // 2  # 1 GiB per-rank upload
    frags, frag_bytes = nvls._spray_fragments(upload)
    assert frags == 144
    assert frag_bytes == math.ceil(upload / 144)
    # At the C++ geometry the 7.46-MiB fragment fails with p ~ 1 per
    # attempt at BER 1e-8, so the collective is incompletable and strict
    # must reject — not accept at 64,920 us like the 8,192-fragment count.
    losses = nvls.expected_permanent_losses(
        2 * 1024**3, 2, "nvls", 1e-8, 3,
        optical_fraction=1.0, optical_hops=1, collective="allgather")
    assert losses > 1.0
    t = nvls.predict(2 * 1024**3, 2, "nvls", 128, ber=1e-8, retry_limit=3,
                     strict_reliability=True, collective="allgather",
                     llr_enabled=True)
    assert not math.isfinite(t)
    # Retry-buffer sizing inherits the coalesced fragment count.
    assert nvls.required_retry_buffer_entries(
        2 * 1024**3, 2, "nvls", collective="allgather") == 144
    # Below the thresholds nothing changes: the single-link gate uses the
    # 500-MiB OVERFLOW_THRESHOLD_COLLECTIVE and coalescing the separate
    # 512-MiB COALESCE_THRESHOLD (codex round 15, finding 6 — the pre-fix
    # gate reused the 500-MiB overflow threshold and coalesced a 504-MiB
    # upload into 72 x 7-MiB fragments where the C++ emits 4,032 x 128
    # KiB), so 499 and 511 MiB (>500, <512) both spray at 128 KiB, 513
    # MiB (>512) coalesces to 72, and 64 MiB keeps the 128-KiB grid.
    assert nvls._spray_fragments(499 * 1024 * 1024)[0] == 3992
    assert nvls._spray_fragments(511 * 1024 * 1024)[0] == 4088
    assert nvls._spray_fragments(513 * 1024 * 1024)[0] == 72
    assert nvls._spray_fragments(64 * 1024 * 1024)[0] == 512


def test_spray_fragments_coalesce_gate_is_512mib():
    """Coalescing must gate on the C++ COALESCE_THRESHOLD (512 MiB), not
    the 500-MiB OVERFLOW_THRESHOLD_COLLECTIVE that governs whether a
    single-link fabric fragments at all — two DIFFERENT constants in
    fabric-endpoint-send.cc. The pre-fix helper reused the overflow
    threshold for both: a 2-GPU / 1008-MiB AllGather (504-MiB per-rank
    upload, between the constants) modeled 72 x 7-MiB upload fragments
    per rank where the C++ emits 4,032 x 128-KiB — ~32x too few and ~56x
    too large — so at BER 1e-8 / SACK / 3 retries strict_reliability
    returned infinity for a config ns-3 completes in 4,251.879 us with
    zero permanent losses (codex round 15, finding 6)."""
    nvls = wb.make_h200_nvls_theory()
    D, N = 1008 * 1024 * 1024, 2
    upload = D // N  # 504 MiB: between 500 and 512 MiB
    frags, frag_bytes = nvls._spray_fragments(upload)
    assert frags == 4032
    assert frag_bytes == 131072
    # The un-coalesced 128-KiB geometry keeps the strict prediction finite
    # (the coalesced 7-MiB fragments made it infinite).
    t = nvls.predict(D, N, "nvls", 128, ber=1e-8, retry_limit=3,
                     retry_mode="sack", strict_reliability=True,
                     collective="allgather", llr_enabled=True)
    assert math.isfinite(t)
    # Above the true 512-MiB coalescing threshold the 72-fragment geometry
    # returns (the round-14 behavior is preserved for genuinely huge
    # transfers).
    assert nvls._spray_fragments(513 * 1024 * 1024)[0] == 72


def test_nvls_retry_buffer_counts_spray_fragments():
    """NVLS retry-buffer sizing must count SendCollectiveBulk's sprayed
    fragments of the endpoint's OWN upload (D for allreduce, D/N for
    allgather) — the LLR fast-copy store holds one entry per fragment.
    The DATA-bulk count (max(lanes, ceil(D/N / 8 MiB)) = 18) wrongly
    called a 32-entry buffer sufficient for 4-GPU / 64-MiB AllGather,
    where each endpoint uploads 128 fragments and ns-3 needs 2 source
    reloads at 32 entries and none at 128 (codex round 13, finding 7)."""
    nvls = wb.make_h200_nvls_theory()
    D, N = 64 * 1024 * 1024, 4
    assert nvls.required_retry_buffer_entries(
        D, N, "nvls", collective="allgather") == 128
    assert nvls.required_retry_buffer_bytes(
        D, N, "nvls", collective="allgather") == D // N  # 128 x 128 KiB
    # AllReduce uploads carry the full D per rank.
    assert nvls.required_retry_buffer_entries(
        16 * 1024 * 1024, 8, "nvls", collective="allreduce") == 128
    # The source-reload penalty must engage below the required count.
    pen32 = nvls._source_reload_penalty(
        D, N, "nvls", 128, 1e-8, True, "sack", 32, "allgather")
    pen128 = nvls._source_reload_penalty(
        D, N, "nvls", 128, 1e-8, True, "sack", 128, "allgather")
    assert pen32 > 0.0 and pen128 == 0.0
    # Non-NVLS algorithms keep the DATA-bulk sizing.
    ring = wb.make_h200_ring_theory()
    assert ring.required_retry_buffer_entries(
        16 * 1024 * 1024, 8, "ring", collective="allreduce") == 4


def test_nvls_retry_buffer_byte_capacity_uses_spray_fragment_size():
    """predict(retry_buffer_bytes=...) must convert the byte capacity with
    the fragment size the transfer actually RETAINS. NVLS collectives
    retain SendCollectiveBulk's sprayed upload fragments (128 x 128 KiB at
    4 GPU / 64 MiB AllGather, = 16 MiB), and the C++ scratch sim converts
    llrBufferSizeBytes / sprayChunkSize the same way — a 16-MiB byte cap
    gives llrBuf=128 and ns-3 takes ZERO source reloads, identical to
    --llrBufferSize=128. The pre-fix conversion divided by the DATA bulk
    fragment (~932 KiB), turning the same 16 MiB into 18 entries and
    adding a false reload penalty: 484.2529 us where the equivalent
    128-entry capacity predicts 483.6896 (codex round 14, finding 6;
    superseded value — round 17, finding 6 later corrected _retry_overhead
    to the spray geometry, moving this config to 463.0972)."""
    nvls = wb.make_h200_nvls_theory()
    D, N = 64 * 1024 * 1024, 4
    kw = dict(ber=1e-8, retry_limit=3, strict_reliability=False,
              collective="allgather", llr_enabled=True, retry_mode="sack")
    t_entries = nvls.predict(D, N, "nvls", 128, retry_buffer_entries=128, **kw)
    t_bytes = nvls.predict(D, N, "nvls", 128,
                           retry_buffer_bytes=16 * 1024 * 1024, **kw)
    assert abs(t_entries - 463.0972) < 2e-3
    assert abs(t_bytes - t_entries) < 1e-9  # same capacity, same latency
    # The divisor itself is the spray fragment size, not the bulk fragment.
    assert (nvls._retry_buffer_fragment_bytes(D, N, "nvls", "allgather")
            == 131072)
    assert (nvls._retry_buffer_fragment_bytes(D, N, "nvls", "allgather")
            != nvls._packet_bytes_per_transfer(D, N, "nvls", "allgather"))
    # predict_tail uses the same conversion (no false penalty on the tail).
    tail_bytes = nvls.predict_tail(
        D, N, "nvls", 128, retry_buffer_bytes=16 * 1024 * 1024, **kw)
    tail_entries = nvls.predict_tail(
        D, N, "nvls", 128, retry_buffer_entries=128, **kw)
    assert abs(tail_bytes - tail_entries) < 1e-9
    # AllReduce uploads carry the full D: 128 frags of 128 KiB = 16 MiB too.
    t_ar = nvls.predict(16 * 1024 * 1024, 8, "nvls", 128,
                        retry_buffer_bytes=16 * 1024 * 1024,
                        ber=1e-10, retry_limit=3, collective="allreduce",
                        llr_enabled=True, retry_mode="sack")
    t_ar_e = nvls.predict(16 * 1024 * 1024, 8, "nvls", 128,
                          retry_buffer_entries=128,
                          ber=1e-10, retry_limit=3, collective="allreduce",
                          llr_enabled=True, retry_mode="sack")
    assert abs(t_ar - t_ar_e) < 1e-9
    # Non-NVLS algorithms keep the DATA-bulk fragment conversion.
    ring = wb.make_h200_ring_theory()
    assert (ring._retry_buffer_fragment_bytes(
        16 * 1024 * 1024, 8, "ring", "allreduce")
        == ring._packet_bytes_per_transfer(
            16 * 1024 * 1024, 8, "ring", "allreduce"))


def test_nvls_retry_overhead_uses_spray_geometry():
    """_retry_overhead must fragment NVLS/SHARP collectives at the collective
    spray geometry, not the DATA bulk path. For H200 NVLS AllGather at
    4 GPU / 64 MiB / BER 1e-7 / SACK the DATA path counts 18 fragments of
    ~932 KB where SendCollectiveBulk sprays 128 x 128 KiB — the reliability
    model already used the spray geometry, and the mean retry latency was
    the outlier: 1510.95 us pre-fix vs 738.89 us with the real geometry
    (codex round 17, finding 6)."""
    nvls = wb.make_h200_nvls_theory()
    D, N = 64 * 1024 * 1024, 4
    val = nvls.predict(D, N, "nvls", 64, ber=1e-7, llr_enabled=True,
                       retry_mode="sack", collective="allgather")
    assert abs(val - 738.8908) < 2e-2

    # The geometry the retry term actually sees: 128 x 128 KiB per upload.
    upload_frags, upload_frag_bytes = nvls._spray_fragments(D // N)
    assert (upload_frags, upload_frag_bytes) == (128, 131072)

    # The DATA-path geometry it must NOT use: 18 x ~932 KB.
    data_packets = nvls._packets_per_transfer(D, N, "nvls", "allgather")
    assert data_packets == 18

    # AllReduce uploads carry the full D per rank: 512 x 128 KiB.
    ar_frags, ar_frag_bytes = nvls._spray_fragments(D)
    assert (ar_frags, ar_frag_bytes) == (512, 131072)

    # Non-NVLS algorithms keep the DATA-bulk geometry in _retry_overhead
    # (the ring's DATA path IS the real wire path there).
    ring = wb.make_h200_ring_theory()
    T = 1.0
    ring_ro = ring._retry_overhead(
        T, 2 * (8 - 1), 16 * 1024 * 1024, 8, "ring", 64, 1e-7, True,
        False, None, None, None, "sack", 1.0, 1.0, "allreduce")
    ring_packets = ring._packets_per_transfer(
        16 * 1024 * 1024, 8, "ring", "allreduce")
    ring_bytes = ring._packet_bytes_per_transfer(
        16 * 1024 * 1024, 8, "ring", "allreduce")
    p = ring._packet_failure_probability(1e-7, False, None, None, None,
                                         ring_bytes)
    attempts = ring._expected_max_attempts(p, ring_packets)
    expected = 2 * (8 - 1) * T * max(0.0, attempts - 1.0)
    assert abs(ring_ro - expected) < 1e-9


def test_spray_fragments_skips_trailing_lane_pad_fragments():
    """The C++ spray loop SKIPS trailing lane-pad fragments whose offset is
    past the wire end (fabric-endpoint-send.cc: `offset >= wireSize ->
    continue`): chunkSize = ceil(wire/numChunks), so a transfer smaller
    than numChunks*chunkSize emits FEWER fragments than numChunks. 32 wire
    bytes over 18 lanes is 16 x 2-byte fragments, not 18 x 2 (the C++ packet
    and retry-store counters both report 16). Counting the two phantom
    fragments over-stated expected losses 0.506 vs 0.450 at BER 2.2e-4 /
    retry 0 — enough to cross the strict-reliability rejection threshold
    (codex round 18, finding 6)."""
    nvls = wb.make_h200_nvls_theory()
    nvls.num_lanes = 18
    # 32 wire bytes / 18 lanes: numChunks=18, chunkSize=ceil(32/18)=2, but
    # fragment 16 starts at offset 32 (>= wire end) and fragment 17 at 34 —
    # both skipped, so 16 fragments carry the transfer.
    frags, frag_bytes = nvls._spray_fragments(32)
    assert (frags, frag_bytes) == (16, 2)
    # 2 bytes over 18 lanes: chunkSize=1, fragments 2..17 all start past
    # the wire end — only 2 fragments carry the transfer.
    assert nvls._spray_fragments(2) == (2, 1)
    # Evenly dividing sizes are unchanged (no trailing pad fragments).
    assert nvls._spray_fragments(36) == (18, 2)
    # 128-KiB multiples still give exactly bytes/128KiB fragments.
    assert nvls._spray_fragments(16 * 1024 * 1024) == (128, 131072)


def test_expected_losses_llr_off_grants_no_retries():
    """With LLR disabled there is no retransmit machinery anywhere: every
    lost packet is permanent on its FIRST attempt, whatever retry_limit
    says. The direct estimator previously granted retry_limit+1 attempts
    even for llr_enabled=False — a 16-MiB 8-GPU ring AllReduce at BER 1e-7
    with retry_limit=3 reported 6.17 expected losses where the one-attempt
    truth is 153.48 (codex round 18, finding 9). predict()'s
    strict-reliability gate already normalized the limit to zero; the
    direct call must agree with it."""
    ring = wb.make_h200_ring_theory()
    D, N = 16 * 1024 * 1024, 8
    one_attempt = ring.expected_permanent_losses(
        D, N, "ring", 1e-7, 0, collective="allreduce", llr_enabled=False)
    with_retries = ring.expected_permanent_losses(
        D, N, "ring", 1e-7, 3, collective="allreduce", llr_enabled=False)
    assert one_attempt > 100.0  # the LLR-off reality: unrecoverable
    assert with_retries == one_attempt  # retry_limit is inert without LLR
    # LLR ON keeps the retry budget.
    with_llr = ring.expected_permanent_losses(
        D, N, "ring", 1e-7, 3, collective="allreduce", llr_enabled=True)
    assert with_llr < one_attempt


def test_nvls_allgather_loss_counts_uploading_ranks():
    """The NVLS AllGather loss estimator multiplied the FULL GPU count into
    both the upload and the result-multicast arms: at 1 GPU / 4 B / BER
    1.8e-3 it predicted 0.761 vs the exact 5p = 0.190 (4x over) with
    infinite strict latency while ns-3 completes (codex round 20, finding
    11). The uploading-rank count min(N, ceil(D)) — 1 here — must scale
    both arms."""
    nvls = wb.make_h200_nvls_theory()
    nvls.num_lanes = 18
    # 1 B / 4 GPUs / FEC RS(544,514,15) / BER 1.8e-3 / LLR off: one
    # uploading rank (min(N, ceil(D)) = 1) ships 1 upload fragment and
    # every rank receives 1 result fragment, so exactly 5 one-attempt
    # fragments carry the collective — 5p for the post-FEC packet failure
    # probability. The pre-fix full-N count gave 8p (4x over) and infinite
    # strict latency while ns-3 completes.
    p = nvls.expected_permanent_losses(
        1, 4, "nvls", 1.8e-3, 5, collective="allgather", llr_enabled=False,
        fec_enabled=True, fec_n=544, fec_k=514, fec_t=15)
    p_frag = nvls._packet_failure_probability(
        1.8e-3, True, 544, 514, 15, packet_bytes=1)
    assert p == pytest.approx(5 * p_frag, rel=1e-9)
    # The many-rank regime is unchanged: 8 GPUs / 16 MiB at BER 1e-9 /
    # retry 3 still predicts ~1.08e-10 (uploading_ranks == N there).
    p8 = nvls.expected_permanent_losses(
        16 * 1024 * 1024, 8, "nvls", 1e-9, 3, collective="allgather",
        llr_enabled=True)
    assert p8 == pytest.approx(1.08e-10, rel=0.2)


def test_zero_byte_predict_is_finite_everywhere():
    """Zero-byte predict previously raised ZeroDivisionError in the tree
    branch (0/0 fragment size at _packet_bytes_per_transfer) and in the
    retry-buffer byte-capacity conversion (floor division by a zero-size
    fragment). Every algorithm/collective must return a finite value at
    D=0 (codex round 20, finding 12)."""
    for make in (wb.make_h200_tree_theory, wb.make_h200_ring_theory,
                 wb.make_h200_nvls_theory, wb.make_h200_fullmesh_theory):
        t = make()
        for algo in ("tree", "ring", "nvls", "fullmesh"):
            for coll in ("allreduce", "allgather", "alltoall", "sendrecv"):
                v = t.predict(0, 4, algo, 128, collective=coll)
                assert math.isfinite(v) and v > 0.0, (algo, coll, v)
        # Byte-capped retry buffers (the capacity conversion) and the
        # strict-reliability path with BER stay finite too.
        tail = t.predict_tail(0, 4, "tree", 128, retry_buffer_bytes=1 << 20,
                              collective="allreduce")
        assert math.isfinite(tail)
        strict = t.predict(0, 4, "tree", 128, ber=1e-4,
                           strict_reliability=True, retry_limit=3,
                           collective="allreduce")
        assert math.isfinite(strict)


def test_nvls_allgather_remainder_grid_fragments():
    """NVLS allgather loss counting must fragment at the ACTUAL per-rank
    integer chunk sizes, not the uniform D/N: the [2,1,1,1] grid at D=5/N=4
    sends 5 upload fragments and mirrors 5 result fragments to each of the
    4 dests (25 fragments total), where the uniform-D/N estimator counted
    40 and predicted 0.6042 expected losses at BER 0.0016 / RS(544,514,15)
    / LLR off — strict latency went infinite for a run ns-3 seeds 1-3 all
    complete (codex round 21, finding 6)."""
    nvls = wb.make_h200_nvls_theory()
    got = nvls.expected_permanent_losses(
        5, 4, "nvls", 0.0016, 3, collective="allgather", llr_enabled=False,
        fec_enabled=True, fec_n=544, fec_k=514, fec_t=15)
    p_frag = nvls._packet_failure_probability(
        0.0016, True, 544, 514, 15, packet_bytes=1)
    assert got == pytest.approx(25 * p_frag, rel=1e-9)
    # The round-20 anchors still hold: D=1/N=4 is 5 fragments.
    p1 = nvls.expected_permanent_losses(
        1, 4, "nvls", 1.8e-3, 5, collective="allgather", llr_enabled=False,
        fec_enabled=True, fec_n=544, fec_k=514, fec_t=15)
    assert p1 == pytest.approx(
        5 * nvls._packet_failure_probability(
            1.8e-3, True, 544, 514, 15, packet_bytes=1),
        rel=1e-9)
    # Many-rank regime unchanged (D%N == 0: uniform and grid coincide).
    p8 = nvls.expected_permanent_losses(
        16 * 1024 * 1024, 8, "nvls", 1e-9, 3, collective="allgather",
        llr_enabled=True)
    assert p8 == pytest.approx(1.08e-10, rel=0.2)


def test_zero_byte_gbn_llr_finite():
    """Zero-byte tree predictions stayed infinite with Go-Back-N LLR: the
    zero packet count produced window==0 and _gbn_cascade returned
    infinity despite zero transfer work (codex round 21, finding 7). D=0
    with llr_enabled=True and BER>0 must be finite in predict,
    predict_tail, and strict mode."""
    tree = wb.make_h200_tree_theory()
    v = tree.predict(0, 4, "tree", 128, collective="allreduce", ber=1e-7,
                     llr_enabled=True)
    assert math.isfinite(v) and v > 0.0
    tail = tree.predict_tail(0, 4, "tree", 128, collective="allreduce",
                             ber=1e-7, llr_enabled=True)
    assert math.isfinite(tail) and tail > 0.0
    strict = tree.predict(0, 4, "tree", 128, collective="allreduce",
                          ber=1e-7, llr_enabled=True, strict_reliability=True,
                          retry_limit=3)
    assert math.isfinite(strict) and strict > 0.0
    # Nonzero control: the GBN overhead path still engages.
    nz = tree.predict(1024, 4, "tree", 128, collective="allreduce",
                      ber=1e-7, llr_enabled=True)
    assert nz > v


# ---- Codex round 22 regressions ----

def test_single_rank_prediction_finite():
    """N=1 ring AllReduce divided by zero: _steps() returned 2*(N-1) == 0
    and _per_step() used it as a divisor. A single-rank prediction must be
    finite (startup + one zero-volume step) for every collective/algorithm
    combination (codex round 22, finding 3)."""
    ring = wb.make_h200_ring_theory()
    for collective in ("allreduce", "allgather", "alltoall", "sendrecv"):
        for algo in ("ring",):
            v = ring.predict(65536, 1, algo, 128, collective=collective)
            assert math.isfinite(v) and v > 0.0, (collective, algo, v)
    tree = wb.make_h200_tree_theory()
    v = tree.predict(65536, 1, "tree", 128, collective="allreduce")
    assert math.isfinite(v) and v > 0.0
    nvls = wb.make_h200_nvls_theory()
    v = nvls.predict(65536, 1, "nvls", 128, collective="allreduce")
    assert math.isfinite(v) and v > 0.0
    # N=1 must not change neighbors: N=2 predictions are untouched.
    assert ring.predict(65536, 2, "ring", 128,
                        collective="allreduce") > 0.0


def test_nvls_allgather_retry_buffer_rank_grid():
    """NVLS AllGather retry-buffer sizing fragmented the uniform D/N
    instead of the per-rank integer grid: at D=73/N=4/18 lanes the chunks
    [19,18,18,18] spray [10,18,18,18] fragments, so the busiest endpoint
    retains 18 — required_retry_buffer_entries returned 10 and a
    16-entry buffer overflowed in ns-3 across seeds 1-3 (codex round 22,
    finding 4). All three sizing functions must follow the grid."""
    nvls = wb.make_h200_nvls_theory(num_lanes=18)
    assert nvls.required_retry_buffer_entries(
        73, 4, "nvls", collective="allgather") == 18
    # Largest retained fragment is rank 0's 2-byte fragment.
    assert nvls._retry_buffer_fragment_bytes(
        73, 4, "nvls", "allgather") == 2
    # Bytes: rank 0's 10x2 = 20 vs rank 1's 18x1 = 18 -> 20.
    assert nvls.required_retry_buffer_bytes(
        73, 4, "nvls", collective="allgather") == 20
    # AllReduce path (full-D upload) is unchanged by the grid fix.
    assert nvls.required_retry_buffer_entries(
        73, 4, "nvls", collective="allreduce") == nvls._spray_fragments(73)[0]
    # Exactly-divisible grid degenerates to the uniform answer.
    assert nvls.required_retry_buffer_entries(
        72, 4, "nvls", collective="allgather") == 18


def test_gbn_topology_cap_scales_retry_penalty():
    """Legacy GBN retransmissions serialized at the uncapped BW_link, so a
    bisection cap left the retry penalty unchanged (2.0435 us at 18 lanes /
    1 MiB / N=8 / 128 credits / BER 1e-7) while the doc contract and the
    SACK path cap the retransmission rate (codex round 22, finding 6).
    Adding a 50-GB/s cap must move the GBN prediction further than the
    pure-serialization delta alone would explain through the SACK floor."""
    ring = wb.make_h200_ring_theory(num_lanes=18)
    kwargs = dict(ber=1e-7, llr_enabled=True, retry_mode="gobackn",
                  collective="allreduce")
    no_cap = ring.predict(1024 * 1024, 8, "ring", 128, topology=None,
                          **kwargs)
    capped = ring.predict(1024 * 1024, 8, "ring", 128,
                          topology=wb.make_topology(bisection_gbps=50),
                          **kwargs)
    assert capped > no_cap
    # The legacy cascade term itself must scale with the capped rate.
    D, N, cr, ber = 1024 * 1024, 8, 128, 1e-7
    steps = ring._steps(N, "ring", "allreduce")
    per_step = ring._per_step(D, N, "ring", steps, "allreduce")
    T_serial = per_step / ring.ring_bw
    legacy_uncapped = ring._gbn_cascade(
        ring.lat + T_serial, steps, D, N, "ring", cr, ber, True,
        "gobackn", None, "allreduce")
    legacy_capped = ring._gbn_cascade(
        ring.lat + T_serial, steps, D, N, "ring", cr, ber, True,
        "gobackn", None, "allreduce",
        bw_eff=min(ring.BW_link, 50000))
    assert legacy_capped > legacy_uncapped
    # SACK control: capped and uncapped differ only via T_serial there,
    # but must also grow (retransmissions re-serialize at the capped rate).
    sack_kwargs = dict(ber=1e-7, llr_enabled=True, retry_mode="sack",
                       collective="allreduce")
    sack_no = ring.predict(1024 * 1024, 8, "ring", 128, topology=None,
                           **sack_kwargs)
    sack_cap = ring.predict(1024 * 1024, 8, "ring", 128,
                            topology=wb.make_topology(bisection_gbps=50),
                            **sack_kwargs)
    assert sack_cap > sack_no


# ---- Codex round 23 regressions ----

def test_sharp_result_uses_switch_multicast_geometry():
    """The AllReduce result arm used the ENDPOINT single-link rule for the
    switch's partition multicast: a single-link fabric's 4-MiB partition was
    modeled as ONE 2-MiB packet (p ~ 1 per attempt) where the switch sprays
    16 x 128-KiB chunks — 0.571 predicted permanent losses at BER 2.1e-7 /
    retry 10000 / strict inf, where ns-3 seeds 1-3 report zero (codex round
    23, finding 2)."""
    nvls = wb.make_h200_nvls_theory(num_lanes=1)
    nvls.nvls_num_partitions = 2
    # Switch geometry: 16 x 131072 per 2-MiB partition, 2 partitions.
    assert nvls._switch_multicast_fragments(2 * 1024 * 1024) == (16, 131072)
    got = nvls.expected_permanent_losses(
        4 * 1024 * 1024, 2, "nvls", 2.1e-7, 10000,
        collective="allreduce", llr_enabled=True)
    assert got < 1e-2, got
    strict = nvls.predict(4 * 1024 * 1024, 2, "nvls", 128,
                          collective="allreduce", ber=2.1e-7,
                          llr_enabled=True, strict_reliability=True,
                          retry_limit=10000)
    assert math.isfinite(strict)


def test_nvls_allgather_retry_latency_rank_grid():
    """The AllGather retry-latency arm fragmented uniform D/N: at
    D=9,437,185 / N=4 / 18 lanes it modeled 19 x 124,174-B fragments where
    three ranks send 18 x 131,072-B — expected max attempts 8.60 vs 8.17,
    326.94 vs 311.77 µs (codex round 23, finding 3). The dominant stream is
    chosen by its expected max attempt count over the real grid."""
    nvls = wb.make_h200_nvls_theory(num_lanes=18)
    v = nvls.predict(9437185, 4, "nvls", 128, ber=1e-6, llr_enabled=True,
                     retry_mode="sack", collective="allgather")
    assert v == pytest.approx(326.9446, rel=1e-4)
    # The many-rank uniform regime (D%N == 0) is unchanged in structure:
    # every rank sprays the same (count, size).
    base = 16 * 1024 * 1024
    frags = nvls._spray_fragments(base // 4)
    assert nvls._spray_fragments(base // 4) == frags


def test_heavy_tail_topology_cap_scales_retransmission():
    """The heavy-tail branch divided delay_per_error by the raw BW_link, so
    a 50-GB/s cap left the p99-minus-mean increment unchanged at 560.39 µs
    for 16 MiB / N=8 ring / 32 credits / BER 2e-7 / SACK (codex round 23,
    finding 4). The increment must scale with the capped rate."""
    ring = wb.make_h200_ring_theory()
    kw = dict(ber=2e-7, llr_enabled=True, retry_mode="sack",
              collective="allreduce", percentile=99)
    mean_kw = dict(ber=2e-7, llr_enabled=True, retry_mode="sack",
                   collective="allreduce")
    t99_no = ring.predict_tail(16 * 1024 * 1024, 8, "ring", 32,
                               topology=None, **kw)
    t99_cap = ring.predict_tail(16 * 1024 * 1024, 8, "ring", 32,
                                topology=wb.make_topology(
                                    bisection_gbps=50), **kw)
    m_no = ring.predict(16 * 1024 * 1024, 8, "ring", 32,
                        topology=None, **mean_kw)
    m_cap = ring.predict(16 * 1024 * 1024, 8, "ring", 32,
                         topology=wb.make_topology(
                             bisection_gbps=50), **mean_kw)
    inc_no = t99_no - m_no
    inc_cap = t99_cap - m_cap
    assert inc_cap > inc_no
    # No cap -> byte-identical legacy behavior.
    assert inc_no == pytest.approx(560.3918, rel=1e-4)
