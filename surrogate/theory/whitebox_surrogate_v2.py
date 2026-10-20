#!/usr/bin/env python3
"""Factorized analytical surrogate model for GPU fabric latency.

The model separates protocol startup and fixed per-step latency from packet
transfer work. Flow control, coding, and shared-resource terms scale only the
transfer work; link-level retransmission and source reload remain additive.
The H200 profile uses three schedule-throughput quantities and two
schedule-specific credit-window quantities. This is the same profile budget as
the previous link-bandwidth, switch-efficiency, credit-batch formulation.

Key derivations (see WHITEBOX_SURROGATE.md for the complete equations):

1. Credit pressure:
   Phi_credit = max(1, C_bdp,a / C)
   C_bdp,a is the packets required to keep schedule a's transfer path busy.

2. Link-level retransmission:
   A protocol step completes after its slowest required packet fragment.
   SACK therefore uses the expected maximum of geometric attempt counts.
   GBN also accounts for the issued suffix of the active credit window and is
   constrained never to predict less delay than SACK.

3. Schedule transfer work:
   T_ring = D(N-1)/(N B_ring)
   T_tree = alpha(N) * D / B_tree     (structural alpha(N) = 1/2/3 by N)
   T_nvls = 2D/B_nvls

4. Shared-resource contention:
   Phi_share = 1 / (1 - rho_eff)
   rho_eff = competing bandwidth / schedule bottleneck bandwidth.

5. Tail latency (stochastic extension):
   Var_step = Var_retx + Var_queue
   p99 = mean + z_99 × sqrt(steps × Var_step)  [CLT, low BER]
   p99 = mean + steps × F_step_inv(0.99^(1/steps))  [EVT, high BER]
"""

import math
from dataclasses import dataclass


@dataclass
class Topology:
    """Optional fabric-topology descriptor for ``predict(topology=...)``.

    Captures the two physical quantities by which the interconnect topology
    influences latency, so the theory model (otherwise calibrated to an ideal
    non-blocking NVSwitch fabric) can honor a real fabric:

    * ``bisection_bw_bytes_per_us`` caps the schedule's effective serialization
      rate via ``min(<algo_bw>, bisection)``. A non-blocking NVSwitch fabric has
      effectively infinite bisection (no cap); a ring / 2-D torus has a tight
      bisection that caps the per-GPU usable bandwidth down.
    * ``hop_count`` adds ``(hop_count - 1) * link_latency`` of per-step link
      propagation. The default 1.0 (one switch traversal) reproduces the
      ideal-fabric model; a k-tier fat-tree / leaf-spine sets it to its depth.

    Units are bytes/µs (matching ``ring_bw`` etc.); ``make_topology`` accepts the
    more natural GB/s. Defaults (``Topology()``) reproduce ``topology=None``,
    so omitting the descriptor is byte-identical to the calibrated model.
    """
    bisection_bw_bytes_per_us: float = float('inf')
    hop_count: float = 1.0


class TheoryDerivedSurrogate:
    """Mechanism-structured latency surrogate with a fixed hardware profile.

    Hardware is supplied at construction. The ``make_*_theory()`` factories bake
    in the calibrated H200 / 1024-optical values; pass ``**overrides`` to a
    factory to define custom hardware on top of that anchor
    (``make_h200_ring_theory(num_lanes=8, startup_us=7)``), or construct this
    class directly for a from-scratch profile. Every parameter below is an
    ``__init__`` keyword (units shown), with the H200 ring value in parentheses
    and the latency path it affects:

      Bandwidth — serialized-bytes term (bytes/µs):
        packet_bw_bytes_per_us         (177000)  retransmission chunk serialize (ber>0)
        ring_bw_bytes_per_us           (177000)  ring per-step transfer        [ring mean]
        tree_bw_per_level_bytes_per_us (275400)  tree GPU<->NVSwitch trunk     [tree mean]
        nvls_bw_bytes_per_us           (535500)  NVLS aggregate transfer       [nvls mean]
      Latency — fixed term (µs unless noted):
        link_latency_us                (0.4 = 400 ns) per-hop propagation  [ring/nvls mean]
        t_min_switch_us                 (4.10)   NVSwitch fixed pipeline cost [tree mean]
        startup_us                      (15.0)   protocol startup (LL/LL128) [all mean]
        nvls_startup_us                 (23.0)   NVLS startup (used for nvls) [nvls mean]
        fec_encode_ns / fec_decode_ns   (50 / 80 ns) FEC pipeline latency   [FEC on]
      Flow control:
        credit_bdp_ring_packets         (91.5)   ring credit BDP -> Phi_credit [ring mean]
        credit_bdp_tree_packets         (35.5)   tree credit BDP -> Phi_credit [tree mean]
      Retransmission / FEC surface (active only at ber>0 or with fec/llr on):
        spray_chunk_bytes               (128 KiB) spray chunk size
        bulk_chunk_bytes                (8 MiB)   bulk-transfer / retry-buffer sizing
        num_lanes                        (4 ring; 18 tree, nvls) NVLink lanes/GPU
        fec_n / fec_k / fec_t            (544 / 514 / 15) RS code
        fec_symbol_bits                  (10)     GF(2^10) symbol
        retry_source_bw_bytes_per_us     (4.8e6)  source-reload bandwidth
        retry_source_latency_us          (0.3)    source-reload latency

    ``predict()`` (mean) is the published path; ``predict_tail()`` (CLT/Pareto,
    not in the paper) lazily imports scipy.stats.
    """

    def __init__(self,
                 packet_bw_bytes_per_us=177000,
                 ring_bw_bytes_per_us=177000,
                 tree_bw_per_level_bytes_per_us=275400,
                 nvls_bw_bytes_per_us=535500,
                 fullmesh_bw_bytes_per_us=449890,
                 link_latency_us=0.4,
                 credit_bdp_ring_packets=91.5,
                 credit_bdp_tree_packets=35.5,
                 spray_chunk_bytes=131072,
                 bulk_chunk_bytes=8 * 1024 * 1024,
                 num_lanes=18,
                 t_min_switch_us=4.10,
                 fec_n=544, fec_k=514, fec_t=15,
                 fec_symbol_bits=10,
                 fec_encode_ns=50, fec_decode_ns=80,
                 startup_us=15.0,
                 nvls_startup_us=23.0,
                 retry_source_bw_bytes_per_us=4_800_000,
                 retry_source_latency_us=0.3,
                 wire_efficiency=1.0):
        # Fixed profile parameters from hardware, ns-3 configuration, or
        # one-time platform calibration.
        self.BW_link = packet_bw_bytes_per_us
        self.ring_bw = ring_bw_bytes_per_us
        self.tree_bw_per_level = tree_bw_per_level_bytes_per_us
        self.nvls_bw = nvls_bw_bytes_per_us
        self.fullmesh_bw = fullmesh_bw_bytes_per_us
        self.lat = link_latency_us
        self.credit_bdp_ring = credit_bdp_ring_packets
        self.credit_bdp_tree = credit_bdp_tree_packets
        self.chunk_size = spray_chunk_bytes
        self.spray_chunk_bytes = spray_chunk_bytes
        self.bulk_chunk_size = bulk_chunk_bytes
        self.num_lanes = num_lanes
        self.T_min_switch = t_min_switch_us
        # Compatibility alias used by an older diagnostic script.
        self.eta_sw = self.tree_bw_per_level / max(self.ring_bw, 1.0)
        self.fec_n = fec_n
        self.fec_k = fec_k
        self.fec_t = fec_t
        self.fec_symbol_bits = fec_symbol_bits
        self.T_fec_latency = (fec_encode_ns + fec_decode_ns) / 1000
        self.S_startup = startup_us
        self.S_nvls_startup = nvls_startup_us
        self.retry_source_bw = retry_source_bw_bytes_per_us
        self.retry_source_latency = retry_source_latency_us
        # Per-transfer wire efficiency (<1 accounts for L3/L4/transport header
        # overhead, e.g. RoCEv2 Eth+IP+UDP+IB ~0.95). Applied to the serial
        # bandwidth (T_serial /= wire_efficiency); 1.0 is byte-identical to the
        # scale-up model (NVLink has no such header overhead).
        self.wire_efficiency = wire_efficiency

    # ---- Collective / algorithm step table ----

    def _validate_collective(self, collective):
        """Validate the collective name. No inference from ``algo``.

        ``collective`` is a required input to ``predict`` / ``predict_tail``;
        the collective + algorithm together select the step / per-step-volume
        model. This helper only validates the name -- it never guesses the
        collective from the algorithm, so omitting it is an error, not a
        default.
        """
        valid = ('allreduce', 'allgather', 'alltoall', 'sendrecv')
        if collective not in valid:
            raise ValueError(
                "collective must be one of %s, got %r" % (valid, collective))
        return collective

    def _steps(self, N, algo, collective=None):
        """Sequential transfer phases for (collective, algorithm).

        AllReduce: ring 2(N-1), tree 2*floor(log2 N)  (reduce-scatter + gather)
        AllGather: nvls 2                              (scatter-reduce + gather)
        AlltoAll:  ring/collnetdirect N-1 permutation phases; tree ceil(log2 N)
                   recursive-doubling rounds
        Send-Recv: 1                                   (single point-to-point hop)
        """
        collective = self._validate_collective(collective)
        if collective == 'sendrecv':
            return 1
        if collective == 'alltoall':
            if algo == 'tree':
                return max(1, math.ceil(math.log2(max(N, 2))))
            return max(1, N - 1)
        if algo == 'fullmesh':
            # Full-mesh: 2 phases (reduce-scatter + all-gather) for allreduce,
            # 1 phase for allgather. The N-1 per-peer sends in each phase are
            # concurrent on dedicated bundles, not sequential steps (unlike the
            # ring's 2*(N-1) pipelined steps), so the step count is O(1).
            return 2 if collective == 'allreduce' else 1
        if algo == 'tree':
            return 2 * max(1, int(math.log2(N)))
        if algo == 'nvls':
            return 2
        return 2 * (N - 1)

    @staticmethod
    def _tree_trunk_load(N):
        """Busiest GPU<->NVSwitch trunk load, in units of D, for the tree.

        ns-3 ``TreeAllReduce`` (post volume-fix) is a heap-shaped binary tree
        (node i -> children 2i+1, 2i+2) with overlapped reduce-up +
        broadcast-down chunk pipelines; every tree edge carries the full D in
        each direction, and each GPU's single switch trunk carries the sum of
        its tree-edge traffic in the busier direction:

          N <= 2: single edge, load D
          N <= 4: the root receives 2D (two children) and sends 2D
          N >= 5: an interior node moves 2D child + 1D parent traffic

        The residual pipeline-fill cost (credit-window ramp-up + chunk
        pipelines warming up, which decays with D) is absorbed into the fitted
        ``t_min_switch`` / trunk efficiency rather than modeled separately.
        """
        if N <= 2:
            return 1.0
        if N <= 4:
            return 2.0
        return 3.0

    def _per_step(self, D, N, algo, steps, collective=None):
        """Bytes moved in one phase of (collective, algorithm).

        AlltoAll moves D*(N-1)/N per GPU (each GPU ships (N-1)/N of its data,
        split over its N-1 partner phases -> D/N per phase); Send-Recv moves
        the full D in its single hop. AllReduce/AllGather keep the legacy
        volume D*(N-1)/N (ring) or D/N (nvls multicast share). Tree AllReduce
        moves alpha(N)*D/steps: the full-volume chunk-pipelined tree carries
        the whole D over every edge, so the busiest GPU<->NVSwitch trunk
        serializes alpha(N)*D total (see ``_tree_trunk_load``).
        """
        collective = self._validate_collective(collective)
        if collective == 'sendrecv':
            return D
        if algo in ('nvls', 'fullmesh'):
            # nvls: D/N multicast share. fullmesh: each peer's dedicated
            # bundle carries one D/N chunk per phase (the N-1 sends are
            # concurrent, so the per-step serialized volume is D/N, not the
            # ring's D*(N-1)/N).
            return D / N
        if algo == 'tree' and collective == 'allreduce':
            return self._tree_trunk_load(N) * D / steps
        return D * (N - 1) / N / steps

    def _bdp(self, N, algo, collective=None):
        """Packets required to keep the selected schedule path busy."""
        collective = self._validate_collective(collective)
        if collective == 'sendrecv':
            return float('inf')
        if algo == 'ring':
            return self.credit_bdp_ring
        if algo == 'tree':
            return self.credit_bdp_tree
        return float('inf')

    # ---- Step time (T_step) ----

    def _step_time(self, per_step, N, algo, topology=None, collective=None):
        """Per-step transfer time (µs).

        Ring / AlltoAll / Send-Recv: T_step = per_step / B_ring + lat
        (AlltoAll and Send-Recv serialize over the per-link wire rate, same as
        the ring schedule; AlltoAll contention is captured via the topology
        bisection cap, not a separate bandwidth constant.)
        Tree: T_step = T_min_switch + per_step / B_tree  (B_tree is the
        GPU<->NVSwitch trunk rate; per_step already carries the alpha(N)
        trunk-load factor via ``_per_step``)
        NVLS: T_step = (N × per_step) / B_nvls + lat

        With ``topology`` the schedule bandwidth is capped by the fabric
        bisection (``min(<algo_bw>, bisection)``) and ``(hop_count - 1) * lat``
        of multi-hop propagation is added; ``topology=None`` is byte-identical.
        """
        bsec = (topology.bisection_bw_bytes_per_us
                if topology is not None else float('inf'))
        hop = topology.hop_count if topology is not None else 1.0
        extra_lat = (hop - 1.0) * self.lat
        if algo == 'tree':
            bw_tree = min(self.tree_bw_per_level, bsec)
            return self.T_min_switch + extra_lat + per_step / bw_tree
        elif algo == 'nvls':
            return extra_lat + self.lat + (per_step * N) / min(self.nvls_bw, bsec)
        elif algo == 'fullmesh':
            # 1 chunk (D/N) over 1 dedicated per-peer bundle -- no N-fold
            # serialization (unlike nvls, which funnels N chunks through one
            # shared switch port). One hop for fullmesh topology (extra_lat=0).
            return extra_lat + self.lat + per_step / min(self.fullmesh_bw, bsec)
        else:
            return extra_lat + self.lat + per_step / min(self.ring_bw, bsec)

    def _one_hop_step_time(self, per_step, N, algo, topology=None, collective=None):
        """Per-step time for AlltoAll partner phases and Send-Recv single hops.

        Each phase ships its bytes over one link at the per-link serial rate
        (the ring-schedule bandwidth), regardless of ``algo``: the tree/NVLS
        bandwidth forms describe their own collective structures (per-level
        trunk / N-fold multicast amplification) and must NOT apply to a
        one-hop transfer.
        """
        bsec = (topology.bisection_bw_bytes_per_us
                if topology is not None else float('inf'))
        hop = topology.hop_count if topology is not None else 1.0
        return (hop - 1.0) * self.lat + self.lat + per_step / min(self.ring_bw, bsec)

    # ---- Credit pressure (Phi_credit) ----

    def _credit_pressure(self, D, N, algo, cr, collective=None):
        """Sliding-window flow control pressure.

        Credit-controlled paths: Phi = max(1, BDP / cr)
        NVLS: Phi = 1 (the modeled NVLS path bypasses endpoint credits)
        Send-Recv: Phi = 1 (single hop, no pipeline to keep busy)
        """
        collective = self._validate_collective(collective)
        if (collective == 'sendrecv' or algo in ('nvls', 'fullmesh')
                or cr <= 0):
            return 1.0

        bdp = self._bdp(N, algo, collective)
        if bdp == float('inf'):
            return 1.0

        return max(1.0, bdp / cr)

    # ---- FEC ----

    def _fec_params(self, fec_n=None, fec_k=None, fec_t=None):
        return (self.fec_n if fec_n is None else fec_n,
                self.fec_k if fec_k is None else fec_k,
                self.fec_t if fec_t is None else fec_t)

    def _post_fec_ber(self, ber, fec_n=None, fec_k=None, fec_t=None):
        """Effective BER after RS(N,K,T) FEC decoding."""
        if ber <= 0:
            return 0.0
        n_symbols, k_symbols, t_symbols = self._fec_params(fec_n, fec_k, fec_t)
        if n_symbols <= 0 or k_symbols <= 0 or t_symbols < 0:
            return ber
        p_sym = 1 - (1 - ber) ** self.fec_symbol_bits
        p_correctable = 0.0
        for i in range(t_symbols + 1):
            log_comb = (math.lgamma(n_symbols + 1) - math.lgamma(i + 1) -
                        math.lgamma(n_symbols - i + 1))
            if 0 < p_sym < 1:
                log_term = (log_comb + i * math.log(p_sym) +
                            (n_symbols - i) * math.log(1 - p_sym))
                p_correctable += math.exp(log_term)
            elif p_sym == 0:
                p_correctable += 1.0 if i == 0 else 0.0
        p_uncorrectable_cw = max(0, 1 - min(p_correctable, 1.0))
        if p_uncorrectable_cw <= 0:
            return 0.0
        data_bits_per_cw = k_symbols * self.fec_symbol_bits
        n_cw_per_chunk = math.ceil(self.chunk_size * 8 / data_bits_per_cw)
        p_chunk_post = 1 - (1 - p_uncorrectable_cw) ** n_cw_per_chunk
        if p_chunk_post <= 0:
            return 0.0
        return 1 - (1 - p_chunk_post) ** (1 / (self.chunk_size * 8))

    def _fec_bw_overhead(self, fec_n=None, fec_k=None, optical_fraction=1.0):
        n_symbols, k_symbols, _ = self._fec_params(fec_n, fec_k, None)
        if n_symbols <= 0 or k_symbols <= 0:
            return 1.0
        optical_fraction = min(1.0, max(0.0, optical_fraction))
        return 1.0 + optical_fraction * (n_symbols / k_symbols - 1.0)

    def _nccl_protocol_for(self, payload_bytes):
        """NCCL wire protocol the C++ side picks for ``payload_bytes``.

        Mirrors ``NcclProtocolModel::GetProtocolForSize`` (thresholds 8 KiB
        and 2 MiB): LL below 8 KiB, LL128 below 2 MiB, SIMPLE above.
        """
        if payload_bytes < 8192:
            return 'LL'
        if payload_bytes < 2 * 1024 * 1024:
            return 'LL128'
        return 'SIMPLE'

    def _nccl_wire_bytes(self, payload_bytes, protocol):
        """Wire bytes ``NcclProtocolModel::GetWireSize`` emits for a payload.

        LL: 64 data + 64 flag bytes per 128 B line; LL128: 120 data + 8
        header per 128 B line; SIMPLE: payload as-is (efficiency 1.0).
        """
        if protocol == 'LL':
            return math.ceil(payload_bytes / 64) * 128
        if protocol == 'LL128':
            return math.ceil(payload_bytes / 120) * 128
        return payload_bytes

    def _wire_bytes_per_transfer(self, data_bytes, num_gpus, algo,
                                 collective=None):
        """Wire bytes in one protocol transfer before FEC encoding.

        AllReduce/AllGather ring-tree step and AlltoAll partner exchange each
        carry data/num_gpus (D/N) per transfer; Send-Recv carries the full D.
        The tree's chunk-pipelined schedule carries the FULL D over every
        edge (see ``_per_step`` / ``_tree_trunk_load``): its per-transfer
        volume is D, not D/N — reliability must count the bytes actually on
        the wire, or it underestimates expected losses by ~alpha(N).

        The tree also picks its wire protocol from the per-rank chunk D/N
        (``TreeAllReduce`` selects once by m_chunkSize = D/N), not from the
        full per-edge D: e.g. at N=8/D=32 KiB the wire protocol is LL
        (4 KiB per-rank < 8 KiB), so each edge carries ceil(D/64)*128 = 64 KiB
        of LL line framing — LL128 selected by total D would count 35,072 B.
        """
        collective = self._validate_collective(collective)
        if algo == 'tree' and collective == 'allreduce':
            per_rank = max(1, data_bytes // max(1, num_gpus))
            if data_bytes < num_gpus:
                per_rank = data_bytes
            protocol = self._nccl_protocol_for(per_rank)
            return self._nccl_wire_bytes(data_bytes, protocol)
        transfer_bytes = (data_bytes if collective == 'sendrecv'
                          else data_bytes / max(1, num_gpus))
        if algo == 'nvls':
            return transfer_bytes
        protocol = self._nccl_protocol_for(transfer_bytes)
        return self._nccl_wire_bytes(transfer_bytes, protocol)

    def _tree_pipe_chunk(self, data_bytes, num_gpus):
        """``TreeAllReduce`` m_pipeChunkSize: clamp(D/(32*depth), 64 KiB, 8 MiB).

        depth = ceil(log2(N)) (heap-shaped binary tree). The tree quantizes
        each edge's full-D forwarding into ceil(D/pipe) pipe chunks, each
        sprayed across the lanes as one bulk transfer.
        """
        depth = max(1, math.ceil(math.log2(max(2, num_gpus))))
        pipe = data_bytes // (32 * depth)
        return min(max(pipe, 64 * 1024), 8 * 1024 * 1024)

    def _bulk_fragment_count(self, wire_bytes, data_bytes):
        """Fragments ``SendBulkWireTransferSize`` emits for one transfer.

        Mirrors the C++ fragment loops exactly: numChunks =
        max(lanes, ceil(wire/bulk_chunk)); wireChunkSize = ceil(wire/numChunks);
        a fragment is emitted only when BOTH its wire offset is inside the
        wire span AND its data offset is inside the data span (trailing
        lane-pad and wire-padding fragments are skipped). At
        wire == data (SIMPLE, efficiency 1.0) the two filters coincide; under
        LL/LL128 framing (wire > data) the data filter can drop extra trailing
        fragments.
        """
        if wire_bytes <= 0:
            return 1
        num_chunks = max(self.num_lanes,
                         math.ceil(wire_bytes / self.bulk_chunk_size))
        wire_chunk = math.ceil(wire_bytes / num_chunks)
        sent_chunks = math.ceil(wire_bytes / wire_chunk)
        data_chunk = math.ceil(data_bytes / max(1, sent_chunks))
        emitted = 0
        for i in range(num_chunks):
            if i * wire_chunk >= wire_bytes:
                continue  # trailing lane-pad fragment past the wire end
            if i * data_chunk >= data_bytes:
                break  # wire-padding fragment: no data bytes left to send
            emitted += 1
        return max(1, emitted)

    def _spray_fragments(self, data_bytes):
        """(count, bytes) of one SendCollectiveBulk sprayed transfer.

        Mirrors the collective spray loop: numChunks =
        max(lanes, ceil(bytes/spray_chunk)) (at least one fragment per
        lane so the transfer uses the aggregate bandwidth), each fragment
        ceil(bytes/numChunks) — the geometry NvSwitch's result multicast
        mirrors chunk-for-chunk. Distinct from _bulk_fragment_count, which
        models the DATA path's bulk_chunk granularity and protocol framing.
        """
        if data_bytes <= 0:
            return 1, 1
        num_chunks = max(self.num_lanes,
                         math.ceil(data_bytes / self.spray_chunk_bytes))
        frag_bytes = math.ceil(data_bytes / num_chunks)
        return num_chunks, frag_bytes

    def _packets_per_transfer(self, data_bytes, num_gpus, algo, collective=None):
        """Fragments the C++ fabric actually emits for one transfer.

        Non-tree algorithms spray the transfer's wire bytes across the lanes
        in bulk-chunk-sized fragments (at least one per lane). The tree is
        different: each edge's full-D volume moves as ceil(D/pipe) pipe
        chunks, EACH of which is its own sprayed bulk transfer — at
        N=8/D=16 MiB that is 97 chunks x 18 fragments + a 16-fragment tail =
        1744 fragments per edge (ns-3 counts exactly 1744), not 18. Counting
        fragments at the whole-transfer granularity understates expected
        permanent losses by the same ~alpha(N) factor (19.2 vs 8.7e-5 at
        BER 1e-7 / retry 3).
        """
        collective = self._validate_collective(collective)
        if algo == 'tree' and collective == 'allreduce':
            pipe = self._tree_pipe_chunk(data_bytes, num_gpus)
            per_rank = max(1, data_bytes // max(1, num_gpus))
            if data_bytes < num_gpus:
                per_rank = data_bytes
            protocol = self._nccl_protocol_for(per_rank)
            fragments = 0
            remaining = data_bytes
            while remaining > 0:
                chunk = min(pipe, remaining)
                chunk_wire = self._nccl_wire_bytes(chunk, protocol)
                fragments += self._bulk_fragment_count(chunk_wire, chunk)
                remaining -= chunk
            return fragments
        wire_bytes = self._wire_bytes_per_transfer(
            data_bytes, num_gpus, algo, collective)
        return self._bulk_fragment_count(
            wire_bytes,
            data_bytes if collective == 'sendrecv'
            else data_bytes / max(1, num_gpus))

    def _packet_bytes_per_transfer(self, data_bytes, num_gpus, algo,
                                   collective=None):
        wire_bytes = self._wire_bytes_per_transfer(
            data_bytes, num_gpus, algo, collective)
        return wire_bytes / self._packets_per_transfer(
            data_bytes, num_gpus, algo, collective)

    def required_retry_buffer_entries(self, data_bytes, num_gpus, algo,
                                      collective=None):
        """Maximum packets an endpoint may need to retain for one tree step."""
        packets = self._packets_per_transfer(
            data_bytes, num_gpus, algo, collective)
        return 2 * packets if algo == 'tree' else packets

    def required_retry_buffer_bytes(self, data_bytes, num_gpus, algo,
                                    collective=None):
        """Bytes needed to retain the modeled transfer fragments."""
        return (
            self.required_retry_buffer_entries(
                data_bytes, num_gpus, algo, collective)
            * self._packet_bytes_per_transfer(
                data_bytes, num_gpus, algo, collective)
        )

    def _retry_buffer_capacity_entries(self, retry_buffer_entries=None,
                                       retry_buffer_bytes=None,
                                       fragment_bytes=None):
        """Entry capacity of a byte-capped retry buffer.

        ``retry_buffer_bytes`` limits how many RETAINED FRAGMENTS the buffer
        holds, so the conversion divides by the fragment size the transfer
        actually uses (``fragment_bytes``, defaulting to the spray chunk
        size) — not by the 128 KiB spray size unconditionally. Retained
        fragments can be as large as the 8 MiB coalesced chunks (or the
        whole transfer when it fits one fragment), and dividing by the wrong
        size over- or under-counts the reload penalty.
        """
        if retry_buffer_bytes is None:
            return retry_buffer_entries
        frag = self.chunk_size if fragment_bytes is None else fragment_bytes
        byte_limited_entries = max(0, int(retry_buffer_bytes // frag))
        if retry_buffer_entries is None:
            return byte_limited_entries
        return min(retry_buffer_entries, byte_limited_entries)

    def _packet_failure_probability(self, ber, fec_enabled=False, fec_n=None,
                                    fec_k=None, fec_t=None, packet_bytes=None):
        if ber <= 0:
            return 0.0
        packet_bytes = self.chunk_size if packet_bytes is None else packet_bytes
        if not fec_enabled:
            return 1.0 - (1.0 - ber) ** (packet_bytes * 8)

        n_symbols, k_symbols, t_symbols = self._fec_params(
            fec_n, fec_k, fec_t)
        p_sym = 1.0 - (1.0 - ber) ** self.fec_symbol_bits
        p_correctable = 0.0
        for errors in range(t_symbols + 1):
            log_comb = (
                math.lgamma(n_symbols + 1)
                - math.lgamma(errors + 1)
                - math.lgamma(n_symbols - errors + 1)
            )
            log_term = (
                log_comb
                + errors * math.log(max(p_sym, 1e-300))
                + (n_symbols - errors) * math.log(max(1.0 - p_sym, 1e-300))
            )
            p_correctable += math.exp(log_term)
        p_uncorrectable = max(0.0, 1.0 - min(1.0, p_correctable))
        codeword_data_bits = k_symbols * self.fec_symbol_bits
        codewords = math.ceil(packet_bytes * 8 / codeword_data_bits)
        return 1.0 - (1.0 - p_uncorrectable) ** codewords

    def expected_permanent_losses(self, data_bytes, num_gpus, algo, ber,
                                  retry_limit, fec_enabled=False, fec_n=None,
                                  fec_k=None, fec_t=None, optical_fraction=1.0,
                                  optical_hops=1.0, collective=None):
        """Expected packets that exhaust the configured retry limit."""
        if retry_limit is None or retry_limit < 0:
            return 0.0
        packet_bytes = self._packet_bytes_per_transfer(
            data_bytes, num_gpus, algo, collective)
        p_link = self._packet_failure_probability(
            ber, fec_enabled, fec_n, fec_k, fec_t, packet_bytes)
        if p_link <= 0:
            return 0.0
        p_path = 1.0 - (1.0 - p_link) ** max(1.0, optical_hops)
        packets_per_transfer = self._packets_per_transfer(
            data_bytes, num_gpus, algo, collective)
        collective = self._validate_collective(collective)
        # Whole-COLLECTIVE transfer counts: the expected-loss estimate
        # aggregates the risk of EVERY transfer the collective issues, not
        # just the ones one rank participates in. The old per-rank counts
        # (ring: 2(N-1) steps, alltoall: N-1 partners) covered only the
        # local rank's slice, understating expected permanent losses by
        # ~N x -- at N=8/D=16 MiB/BER 2e-7/retry 3 the ring model reported
        # 0.2108 losses against 1.6865 when all ranks were counted, so
        # ``strict_reliability`` wrongly returned a finite latency for a
        # config that cannot complete.
        if collective == 'sendrecv':
            transfer_count = 1
        elif collective == 'alltoall':
            # Every rank exchanges with every other rank: N(N-1) transfers.
            transfer_count = num_gpus * (num_gpus - 1)
        elif collective == 'allreduce':
            if algo == 'nvls':
                # NVLS/SHARP: every rank uploads once and the switch
                # multicasts once -- N uploads + N result deliveries.
                transfer_count = 2 * num_gpus
            elif algo == 'tree':
                # Tree allreduce: the tree has N-1 edges and each carries the
                # FULL D per direction (D up during reduce, D down during
                # broadcast — see _wire_bytes_per_transfer's tree branch,
                # which also counts full-D fragments per edge). The transfer
                # count is therefore 2(N-1), NOT 2N(N-1): the old multiplier
                # overcounted tree losses by N (at N=8/D=16 MiB/BER 6e-7/
                # retry 3 it reported 0.80997 expected losses instead of
                # 0.10125, and strict_reliability wrongly returned infinity).
                transfer_count = 2 * (num_gpus - 1)
            else:
                # Ring/fullmesh allreduce = reduce-scatter + all-gather.
                # Ring: every rank does 2(N-1) chunk steps -> 2N(N-1);
                # fullmesh: every rank sends D/N to N-1 peers per phase
                # -> 2N(N-1).
                transfer_count = 2 * num_gpus * (num_gpus - 1)
        elif collective == 'allgather':
            if algo == 'ring':
                # Ring allgather: every rank passes D/N around the ring
                # through N-1 peers -> N(N-1) transfers.
                transfer_count = num_gpus * (num_gpus - 1)
            elif algo == 'nvls':
                # NVLS allgather: N uploads + N multicast deliveries.
                transfer_count = 2 * num_gpus
            else:
                # Fullmesh allgather: every rank sends D/N to all N-1 peers.
                transfer_count = num_gpus * (num_gpus - 1)
        else:
            transfer_count = num_gpus
        # NVLS/SHARP needs per-SIDE packet counts: uploads and result
        # deliveries fragment differently. Each rank's upload carries D/N
        # (allgather) or D (allreduce); each multicast result delivers the
        # FULL D to every destination, re-fragmented at the upload chunks'
        # granularity — the switch multicasts each received upload chunk, so
        # a dest's result stream has N * frag(D/N) packets. Counting both
        # sides at the upload's packetization understates the result risk:
        # at N=8/D=16 MiB the surrogate counted 288 packets where the C++
        # fabric emits 1,296 (144 upload + 1,152 result), so BER 1e-9/0
        # retries gave 0.2683 expected losses instead of ~1.2074 and
        # strict_reliability stayed finite for a config that cannot complete.
        # The fragment geometry is SendCollectiveBulk's spray, NOT the DATA
        # bulk path: numChunks = max(lanes, ceil(bytes/spray_chunk)) over the
        # ACTUAL side size — a rank uploads D/N (allgather) or D (allreduce)
        # in one sprayed transfer, and each result chunk mirrors one upload
        # chunk (same wire/effective sizes). Calling _packets_per_transfer
        # here double-divided by N (it divides internally, and the branch
        # passed D/N again) and borrowed the DATA path's packet bytes
        # (~932 KB fragments at D=128 MiB, ~1370x the failure probability of
        # the real 128 KB spray chunks): 8 GPU / 128 MiB allgather / BER
        # 5e-8 / retry 3 reported 12.1577 expected losses where the C++
        # geometry gives 0.0627 and the ns-3 run completes with zero
        # permanent losses (codex round 12, finding 6).
        if algo == 'nvls' and collective in ('allreduce', 'allgather'):
            upload_bytes = (data_bytes / max(1, num_gpus)
                            if collective == 'allgather' else data_bytes)
            upload_frags, upload_frag_bytes = (
                self._spray_fragments(upload_bytes))
            # Result side per destination: the switch re-multicasts every
            # received upload chunk, so a dest's stream carries N streams of
            # frag(D/N) chunks at the upload chunk sizes (allreduce results
            # mirror the D-byte upload chunks the same way).
            result_frags_per_dest = num_gpus * upload_frags
            attempts = retry_limit + 1
            p_frag = self._packet_failure_probability(
                ber, fec_enabled, fec_n, fec_k, fec_t, upload_frag_bytes)
            p_path_frag = 1.0 - (1.0 - p_frag) ** max(1.0, optical_hops)
            return ((num_gpus * upload_frags
                     + num_gpus * result_frags_per_dest)
                    * min(1.0, max(0.0, optical_fraction))
                    * p_path_frag ** attempts)
        attempts = retry_limit + 1
        return (transfer_count * packets_per_transfer
                * min(1.0, max(0.0, optical_fraction))
                * p_path ** attempts)

    # ---- Shared-resource contention (Phi_share) ----

    def _shared_contention(self, algo, cr, competing_bw, D, N, collective=None,
                          bisection_bw=None):
        """Residual-capacity penalty at a shared bottleneck.

        ``competing_bw`` is the traffic rate, in GB/s, that uses the same
        bottleneck as the modeled transfer. The expression has no fitted
        contention coefficient.

        The schedule bandwidth must match the serial term it penalizes — two
        dimensions of that match are load-bearing:

        1. The COLLECTIVE dimension: alltoall / sendrecv phases serialize over
           the per-link (ring) rate — their ``predict`` T_serial uses
           ``ring_bw`` regardless of ``algo`` — so the contention divisor must
           too, else ``algo='tree'`` undercuts the penalty (rho computed against
           the faster tree trunk while the transfer actually runs at ring
           rate).
        2. The TOPOLOGY dimension: when a topology caps the serial rate (a
           bisection / egress bottleneck below the fabric's own link rate),
           the competing traffic eats into the CAPPED rate, not the raw link
           rate — a 25 GB/s flow sharing a link capped to 50 GB/s leaves
           25 GB/s of residual, a 2x penalty, not 1.16x. Using the uncapped
           rate understates the penalty exactly when the bottleneck is
           tightest.
        """
        if competing_bw <= 0:
            return 1.0

        collective = self._validate_collective(collective)
        if collective in ('alltoall', 'sendrecv'):
            schedule_bw = self.ring_bw
        elif algo == 'tree':
            schedule_bw = self.tree_bw_per_level
        elif algo == 'nvls':
            schedule_bw = self.nvls_bw
        elif algo == 'fullmesh':
            schedule_bw = self.fullmesh_bw
        else:
            schedule_bw = self.ring_bw
        if bisection_bw is not None:
            schedule_bw = min(schedule_bw, bisection_bw)
        schedule_bw_GB = schedule_bw / 1000.0
        rho = min(0.99, competing_bw / max(schedule_bw_GB, 1e-9))
        return 1.0 / (1.0 - rho)

    # ---- GBN cascade (T_gbn) — derived from protocol waste ----

    @staticmethod
    def _expected_max_attempts(failure_probability, packet_count):
        """Expected attempts of the slowest of parallel geometric trials."""
        if failure_probability <= 0 or packet_count <= 0:
            return 1.0
        if failure_probability >= 1.0:
            return float('inf')
        expected = 0.0
        round_index = 0
        while round_index < 10000:
            tail = 1.0 - (
                1.0 - failure_probability ** round_index
            ) ** packet_count
            expected += tail
            if round_index > 0 and tail < 1e-12:
                break
            round_index += 1
        return expected

    def _retry_overhead(self, T_serial_step, steps, D, N, algo, cr, ber,
                        llr_enabled, fec_enabled=False, fec_n=None, fec_k=None,
                        fec_t=None, retry_mode='gobackn',
                        optical_fraction=1.0, optical_hops=1.0,
                        collective=None):
        """Delay from packet attempts that extend a protocol step.

        A step completes after all of its parallel packet fragments arrive.
        The slowest fragment therefore follows the maximum of geometric
        attempt counts, rather than the mean count of one packet.
        """
        collective = self._validate_collective(collective)
        if ber <= 0 or not llr_enabled:
            return 0.0
        packets = self._packets_per_transfer(D, N, algo, collective)
        packet_bytes = self._packet_bytes_per_transfer(D, N, algo, collective)
        p_packet = self._packet_failure_probability(
            ber, fec_enabled, fec_n, fec_k, fec_t, packet_bytes)
        optical_fraction = min(1.0, max(0.0, optical_fraction))
        error_trials = max(1.0, optical_hops)
        p_path = 1.0 - (1.0 - p_packet) ** error_trials
        p_path *= optical_fraction
        attempts = self._expected_max_attempts(p_path, packets)
        if not math.isfinite(attempts):
            return float('inf')
        extra_rounds = max(0.0, attempts - 1.0)
        if retry_mode.lower() != 'sack':
            bdp = self._bdp(N, algo, collective)
            window = cr if bdp == float('inf') else min(cr, bdp)
            window = min(window, packets)
            extra_rounds *= (window + 1.0) / 2.0
        return steps * T_serial_step * extra_rounds

    def _gbn_cascade(self, T_step, steps, D, N, algo, cr, ber, llr_enabled,
                     retry_mode='gobackn', retry_buffer_entries=None,
                     collective=None):
        """Link-retry overhead (additive, in microseconds).

        Only applies when llr_enabled=True AND ber>0 (after FEC reduction).
        When llr=0, errors cause permanent loss (no retransmission).

        SACK retransmits the failed packet. GBN retransmits the failed packet
        and the suffix already issued in the same protocol transfer. Each
        retransmitted chunk takes chunk_size/BW to serialize.

        T_gbn = P_error_total × (window/2) × (chunk_size/BW) × Phi_absorption

        where:
          P_error_total = total_chunks × P_chunk (expected number of errors)
          window = min(cr, BDP, packets_per_transfer, retry_buffer_entries)
          Phi_overlap = window/(cr + window)
            When cr exceeds BDP, additional credits increase overlap and
            reduce the retransmission delay visible at operation completion.

        The same expected-retransmission expression applies to every collective
        schedule. ``total_chunks`` follows the collective volume: D*(N-1)/N for
        AllReduce/AllGather/AlltoAll, the full D for Send-Recv.
        """
        resolved = self._validate_collective(collective)
        if ber <= 0 or not llr_enabled:
            return 0.0

        bits_per_chunk = self.chunk_size * 8
        P_chunk = 1 - (1 - ber) ** bits_per_chunk
        if P_chunk <= 0:
            return 0.0

        if resolved == 'sendrecv':
            total_chunks = D / self.chunk_size
        else:
            total_chunks = D * (N - 1) / N / self.chunk_size
        P_error_total = total_chunks * P_chunk

        bdp = self._bdp(N, algo, collective)
        if bdp == float('inf'):
            window = cr
            cr_eff = cr
        else:
            window = min(cr, bdp)
            cr_eff = window

        window = min(window, self._packets_per_transfer(D, N, algo, collective))
        if window <= 0:
            return float('inf')

        chunk_serial = self.chunk_size / self.BW_link  # time to send one chunk

        if retry_mode.lower() == 'sack':
            retransmitted_chunks = 1.0
        else:
            retransmitted_chunks = (window + 1.0) / 2.0
        waste_per_error = retransmitted_chunks * chunk_serial

        # Profiled overlap with packets already in flight.
        phi_overlap = cr_eff / (cr + cr_eff) if cr > 0 else 1.0

        return P_error_total * waste_per_error * phi_overlap

    def _source_reload_penalty(self, D, N, algo, cr, ber, llr_enabled,
                               retry_mode, retry_buffer_entries,
                               collective=None):
        """Visible delay from reconstructing packets absent from the retry buffer."""
        resolved = self._validate_collective(collective)
        if ber <= 0 or not llr_enabled or retry_buffer_entries is None:
            return 0.0

        required = self.required_retry_buffer_entries(
            D, N, algo, collective)
        if required <= 0 or retry_buffer_entries >= required:
            return 0.0
        miss_fraction = 1.0 - max(0, retry_buffer_entries) / required

        p_chunk = 1.0 - (1.0 - ber) ** (self.chunk_size * 8)
        if resolved == 'sendrecv':
            total_chunks = D / self.chunk_size
        else:
            total_chunks = D * (N - 1) / N / self.chunk_size
        expected_errors = total_chunks * p_chunk

        bdp = self._bdp(N, algo, collective)
        window = cr if bdp == float('inf') else min(cr, bdp)
        window = min(window, self._packets_per_transfer(D, N, algo, collective))
        retry_chunks = (1.0 if retry_mode.lower() == 'sack'
                        else (window + 1.0) / 2.0)

        source_service = self.retry_source_latency
        if self.retry_source_bw > 0:
            source_service += self.chunk_size / self.retry_source_bw
        cr_eff = min(cr, bdp) if bdp != float('inf') else cr
        visible = cr_eff / (cr + cr_eff) if cr > 0 else 1.0
        return (expected_errors * retry_chunks * miss_fraction
                * source_service * visible)

    # ---- Tail latency (stochastic extension) ----

    def _step_variance(self, T_step, D, N, algo, cr, ber, competing_bw, llr_enabled,
                       fec_enabled=False, fec_n=None, fec_k=None, fec_t=None,
                       retry_mode='gobackn', retry_buffer_entries=None,
                       optical_fraction=1.0, collective=None):
        """Per-step delay variance from retransmission + queuing.

        Uses post-FEC BER when FEC is enabled (variance from corrected errors
        is near-zero; only uncorrectable errors trigger retransmission).
        BER is scaled by optical_fraction to mirror predict()'s retransmission
        arm: only the optical (error-bearing) fraction of traffic retries.

        Pipeline absorption factor: the Binomial variance K*p*(1-p)*Δ²
        computes the FULL retransmission delay variance, but only a fraction
        propagates to inter-step interval variance. A GBN retransmission of
        `window` chunks delays step k, but step k+1 is also delayed (the
        pipeline stalls). The inter-step interval [k,k+1] sees no spike
        because both completions shift by the same Δ. Only the entry and
        exit intervals spike. The propagation fraction is:

            f = T_step / (T_step + effective_delay)

        where effective_delay accounts for credit slack: when cr > BDP,
        spare credits absorb part of the retransmission, reducing the
        effective delay that propagates.
        """
        collective = self._validate_collective(collective)
        effective_ber = (self._post_fec_ber(ber, fec_n, fec_k, fec_t)
                         if fec_enabled else ber)
        effective_ber *= min(1.0, max(0.0, optical_fraction))
        if effective_ber <= 0 and competing_bw <= 0:
            return 0.0

        steps = self._steps(N, algo, collective)
        per_step = self._per_step(D, N, algo, steps, collective)
        K_step = per_step / self.chunk_size

        bdp = self._bdp(N, algo, collective)
        window = min(cr, bdp) if bdp != float('inf') else cr
        window = min(window, self._packets_per_transfer(D, N, algo, collective))
        var = 0.0

        # Retransmission variance (only when LLR enabled)
        if effective_ber > 0 and llr_enabled:
            bits_per_chunk = self.chunk_size * 8
            P_chunk = 1 - (1 - effective_ber) ** bits_per_chunk
            retry_chunks = (1.0 if retry_mode.lower() == 'sack'
                            else (window + 1.0) / 2.0)
            delay_per_error = retry_chunks * self.chunk_size / self.BW_link

            # Pipeline absorption: compute effective delay after credit slack
            # and the propagation fraction.
            if bdp != float('inf') and cr > bdp:
                spare_credits = cr - bdp
                excess_chunks = max(0.0, window - spare_credits)
            else:
                excess_chunks = window
            effective_delay = excess_chunks * self.chunk_size / self.BW_link

            if T_step > 0 and effective_delay > 0:
                f_prop = T_step / (T_step + effective_delay)
            elif effective_delay <= 0:
                f_prop = 0.0  # fully absorbed, no variance propagation
            else:
                f_prop = 1.0

            var += K_step * P_chunk * (1 - P_chunk) * delay_per_error ** 2 * f_prop

        # Queuing variance (M/M/1 Kingman)
        if competing_bw > 0:
            bw_raw_GB = self.BW_link * 1e6 / 1e9
            rho = min(0.99, competing_bw / max(bw_raw_GB, 1))
            mu = self.BW_link / self.chunk_size
            var += rho / ((1 - rho) ** 2 * mu ** 2)

        return var

    def predict_tail(self, data_bytes, num_gpus, algo, credits, ber=0, mem_bw=0,
                     startup_us=None, fec_enabled=False, llr_enabled=False,
                     percentile=99, competing_bw=None, fec_n=None, fec_k=None,
                     fec_t=None, retry_mode='gobackn', retry_buffer_entries=None,
                     retry_buffer_bytes=None,
                     optical_fraction=1.0, optical_hops=1.0,
                     strict_reliability=False, retry_limit=None, topology=None,
                     *, collective):
        """Predict tail latency (pXX) in microseconds."""
        if competing_bw is None:
            competing_bw = mem_bw
        retry_buffer_entries = self._retry_buffer_capacity_entries(
            retry_buffer_entries, retry_buffer_bytes,
            self._packet_bytes_per_transfer(
                data_bytes, num_gpus, algo, collective))
        mean = self.predict(
            data_bytes, num_gpus, algo, credits, ber=ber, mem_bw=mem_bw,
            startup_us=startup_us, fec_enabled=fec_enabled,
            llr_enabled=llr_enabled, competing_bw=competing_bw,
            fec_n=fec_n, fec_k=fec_k, fec_t=fec_t,
            retry_mode=retry_mode, retry_buffer_entries=retry_buffer_entries,
            optical_fraction=optical_fraction, optical_hops=optical_hops,
            strict_reliability=strict_reliability, retry_limit=retry_limit,
            topology=topology, collective=collective)
        if not math.isfinite(mean):
            return mean
        steps = self._steps(num_gpus, algo, collective)
        T_step = self._one_hop_step_time(
            self._per_step(data_bytes, num_gpus, algo, steps, collective),
            num_gpus, algo, topology, collective) \
            if self._validate_collective(collective) in ('alltoall', 'sendrecv') \
            else self._step_time(
                self._per_step(data_bytes, num_gpus, algo, steps, collective),
                num_gpus, algo, topology, collective)
        var_step = self._step_variance(T_step, data_bytes, num_gpus, algo,
                                        credits, ber, competing_bw, llr_enabled,
                                        fec_enabled, fec_n, fec_k, fec_t,
                                        retry_mode, retry_buffer_entries,
                                        optical_fraction, collective)

        if var_step <= 0:
            return mean

        # Scale BER by the optical (error-bearing) fraction, mirroring
        # predict()'s retransmission arm (line ~962): with optical_fraction=0
        # there are no error-bearing hops, so the tail must collapse to the
        # BER-zero value, not inherit the full-BER retry cascade.
        effective_ber = (self._post_fec_ber(ber, fec_n, fec_k, fec_t)
                         if fec_enabled else ber)
        effective_ber *= min(1.0, max(0.0, optical_fraction))
        bits_per_chunk = self.chunk_size * 8
        P_chunk = 1 - (1 - effective_ber) ** bits_per_chunk if effective_ber > 0 else 0
        # The heavy-tail branch models a RETRANSMISSION cascade (expected
        # retry delay per chunk error). With LLR disabled there are no
        # retransmissions — a lost chunk is a permanent failure the mean
        # already handles (strict_reliability) — so BER must not add tail
        # here no matter how large P_chunk is. Gating on llr_enabled mirrors
        # _step_variance's retransmission term, which has the same guard.
        is_heavy_tail = P_chunk > 0.1 and llr_enabled

        if not is_heavy_tail:
            from scipy.stats import norm
            z = norm.ppf(percentile / 100)
            var_total = steps * var_step
            return mean + z * math.sqrt(var_total)
        else:
            bdp = self._bdp(num_gpus, algo, collective)
            window = min(credits, bdp) if bdp != float('inf') else credits
            delay_per_error = window * self.chunk_size / self.BW_link
            # Exponential tail approximating the geometric retry cascade: the
            # expected extra delay is (P/(1-P)) * delay_per_error (retries per
            # success), so the exponential rate is its reciprocal:
            # lam = (1-P)/P / delay_per_error. The old P/(1-P) form inverted
            # the dependence -- a HIGHER loss probability produced a SHORTER
            # tail (p99 shrank 19680 -> 382 us as BER rose 1.1e-7 -> 2e-6).
            if P_chunk < 1:
                lam = (1.0 - P_chunk) / P_chunk / delay_per_error
            else:
                lam = 1e-6  # every chunk fails: tail diverges
            p_target = (percentile / 100) ** (1 / steps)
            step_tail = -math.log(1 - p_target) / lam
            return mean + steps * step_tail

    # ---- Main prediction ----

    def predict(self, data_bytes, num_gpus, algo, credits, ber=0, mem_bw=0,
                startup_us=None, fec_enabled=False, llr_enabled=False,
                competing_bw=None, fec_n=None, fec_k=None, fec_t=None,
                retry_mode='gobackn', retry_buffer_entries=None,
                retry_buffer_bytes=None,
                optical_fraction=1.0, optical_hops=1.0,
                strict_reliability=False, retry_limit=None,
                include_credit=True, include_fec=True,
                include_retransmission=True, include_contention=True,
                topology=None, *, collective, phase='full'):
        """Predict mean collective latency (µs).

        ``collective`` (required) names the operation (``allreduce`` /
        ``allgather`` / ``alltoall`` / ``sendrecv``) and, together with
        ``algo``, selects the step / per-step-volume model. It is NOT inferred
        from ``algo`` -- the caller must pass it explicitly; omitting it raises
        ``TypeError`` and an invalid name raises ``ValueError``. Use
        ``collective='alltoall'`` or ``'sendrecv'`` to model those operations.

        ``phase`` (default ``'full'``) selects a sub-collective pass for the
        hierarchical decomposition: ``'rs'`` (reduce-scatter) / ``'ag'``
        (allgather) halve the step count (a single pass moves the same total
        volume over half the steps, so the serial term is unchanged and only
        the fixed per-step latency halves). ``'full'`` (default) is the
        two-pass allreduce / allgather -- byte-identical to prior behavior.

        L = S_startup
            + steps × T_fixed
            + steps × T_serial × Phi_credit × Phi_fec_bw × Phi_share
            + T_gbn
            + T_reload
            + T_fec_latency
        """
        if competing_bw is None:
            competing_bw = mem_bw
        retry_buffer_entries = self._retry_buffer_capacity_entries(
            retry_buffer_entries, retry_buffer_bytes,
            self._packet_bytes_per_transfer(
                data_bytes, num_gpus, algo, collective))
        optical_fraction = min(1.0, max(0.0, optical_fraction))
        resolved = self._validate_collective(collective)
        S = startup_us if startup_us is not None else (
            self.S_nvls_startup if resolved == 'allgather'
            and algo == 'nvls' else self.S_startup)
        steps = self._steps(num_gpus, algo, collective)
        if phase in ('rs', 'ag'):
            steps = max(1, steps // 2)
        per_step = self._per_step(data_bytes, num_gpus, algo, steps, collective)

        bsec = (topology.bisection_bw_bytes_per_us
                if topology is not None else float('inf'))
        hop = topology.hop_count if topology is not None else 1.0

        if algo == 'tree':
            T_fixed_step = self.T_min_switch + (hop - 1.0) * self.lat
            T_serial_step = per_step / min(self.tree_bw_per_level, bsec)
        elif algo == 'nvls':
            T_fixed_step = self.lat + (hop - 1.0) * self.lat
            T_serial_step = (per_step * num_gpus) / min(self.nvls_bw, bsec)
        elif algo == 'fullmesh':
            T_fixed_step = self.lat + (hop - 1.0) * self.lat
            T_serial_step = per_step / min(self.fullmesh_bw, bsec)
        else:
            T_fixed_step = self.lat + (hop - 1.0) * self.lat
            T_serial_step = per_step / min(self.ring_bw, bsec)
        # AlltoAll partner phases and Send-Recv single hops are ONE-HOP
        # transfers: each phase ships its bytes over one link at that link's
        # serial rate and pays the single-hop fixed latency (lat + extra hops),
        # exactly as _one_hop_step_time (used by predict_tail) models them.
        # The tree/NVLS forms above describe their own collective structures
        # (the tree's per-level T_min_switch pipeline, NVLS's N-fold multicast
        # amplification) and must NOT apply — with algo='nvls' a sendrecv
        # would otherwise scale its single-hop latency by N, and with
        # algo='tree' it would inherit the multi-level switch pipeline's fixed
        # cost.
        if resolved in ('alltoall', 'sendrecv'):
            T_fixed_step = self.lat + (hop - 1.0) * self.lat
            T_serial_step = per_step / min(self.ring_bw, bsec)
        # RoCEv2 (and any L3 fabric) pays per-transfer header overhead: the
        # sender serializes data/wire_efficiency wire bytes (ceil(data/0.95) in
        # the ns-3 RoceProtocolModel). 1.0 (NVLink, no header) is byte-identical.
        T_serial_step = T_serial_step / self.wire_efficiency

        Phi_cr = (self._credit_pressure(
            data_bytes, num_gpus, algo, credits, collective)
            if include_credit else 1.0)
        Phi_fec = (
            self._fec_bw_overhead(fec_n, fec_k, optical_fraction)
            if fec_enabled and include_fec else 1.0)
        Phi_share = (
            self._shared_contention(
                algo, credits, competing_bw, data_bytes, num_gpus, collective,
                bsec)
            if include_contention else 1.0)

        if strict_reliability:
            # With LLR disabled there is no retransmission machinery: every
            # lost packet is permanent on its first attempt, whatever
            # retry_limit says (the effective attempt count is 1). The old
            # `and llr_enabled` gate skipped this check entirely for LLR-off
            # fabrics, letting a run with dozens of expected permanent losses
            # report a finite completion time. The outer `retry_limit is not
            # None` gate had the same hole: with LLR off the effective retry
            # limit is 0 regardless of the parameter, so the check must run
            # even when retry_limit is omitted (the LLR-off default).
            # LLR-on fabrics keep retry_limit's meaning: None means "model
            # default" and the permanent-loss expectation is skipped, exactly
            # as before.
            if not llr_enabled:
                # LLR off: every loss is permanent (no retransmit machinery).
                eff_retry_limit = 0
            elif retry_limit is not None:
                eff_retry_limit = retry_limit
            else:
                eff_retry_limit = None
            if eff_retry_limit is not None:
                expected_losses = self.expected_permanent_losses(
                    data_bytes, num_gpus, algo, ber, eff_retry_limit,
                    fec_enabled and include_fec, fec_n, fec_k, fec_t,
                    optical_fraction, optical_hops, collective)
                if expected_losses >= 0.5:
                    return float('inf')
        T_step = T_fixed_step + T_serial_step
        if include_retransmission:
            effective_ber = (
                self._post_fec_ber(ber, fec_n, fec_k, fec_t)
                if fec_enabled and include_fec else ber)
            effective_ber *= optical_fraction
            if retry_mode.lower() == 'sack':
                T_gbn = self._retry_overhead(
                    T_serial_step, steps, data_bytes, num_gpus, algo, credits,
                    ber, llr_enabled, fec_enabled and include_fec,
                    fec_n, fec_k, fec_t, retry_mode,
                    optical_fraction, optical_hops, collective)
            else:
                T_gbn_legacy = self._gbn_cascade(
                    T_step, steps, data_bytes, num_gpus, algo, credits,
                    effective_ber, llr_enabled, retry_mode,
                    retry_buffer_entries, collective)
                sack_floor = self._retry_overhead(
                    T_serial_step, steps, data_bytes, num_gpus, algo, credits,
                    ber, llr_enabled, fec_enabled and include_fec,
                    fec_n, fec_k, fec_t, 'sack',
                    optical_fraction, optical_hops, collective)
                bdp = self._bdp(num_gpus, algo, collective)
                window = credits if bdp == float('inf') else min(credits, bdp)
                gbn_floor = sack_floor * (1.0 + 1.0 / max(window, 1.0))
                T_gbn = max(T_gbn_legacy, gbn_floor)
            T_reload = self._source_reload_penalty(
                data_bytes, num_gpus, algo, credits, effective_ber, llr_enabled,
                retry_mode, retry_buffer_entries, collective)
        else:
            T_gbn = 0.0
            T_reload = 0.0
        T_fec = (
            self.T_fec_latency * max(0.0, optical_hops)
            if fec_enabled and include_fec else 0.0)

        return (S + steps * T_fixed_step
                + steps * T_serial_step * Phi_cr * Phi_fec * Phi_share
                + T_gbn + T_reload + T_fec)


# ---- Factory functions ----

def make_h200_ring_theory(**overrides):
    """H200 NVLink4 ring profile.

    Optional ``overrides`` replace any hardware parameter listed in
    ``TheoryDerivedSurrogate.__init__`` (e.g.
    ``make_h200_ring_theory(num_lanes=8, startup_us=7)``), so a user can
    define custom hardware on top of the calibrated H200 anchor. Unknown
    keys raise ``TypeError`` from the constructor.
    """
    defaults = dict(
        packet_bw_bytes_per_us=177000,
        ring_bw_bytes_per_us=177000,
        tree_bw_per_level_bytes_per_us=275400,
        nvls_bw_bytes_per_us=535500,
        link_latency_us=0.4,
        credit_bdp_ring_packets=91.5,
        credit_bdp_tree_packets=35.5,
        spray_chunk_bytes=131072,
        bulk_chunk_bytes=8 * 1024 * 1024,
        num_lanes=4,
        t_min_switch_us=4.10,
        startup_us=15.0,
    )
    defaults.update(overrides)
    return TheoryDerivedSurrogate(**defaults)


def make_h200_tree_theory(**overrides):
    """H200 tree profile: chunk-pipelined binary tree through the NVSwitch.

    Calibrated to the post-local-rank-fix ns-3 ``TreeAllReduce`` at the TRUE
    H200 fabric (``--linksPerGpu=18`` separate NVLink4 lanes @ 170 Gbps,
    spray-routed; every tree edge carries the full D; see
    ``configs/calibration/tree_ns3_sweep.py`` / ``tree_calib.json``). TWO
    fitted parameters: ``tree_bw_per_level`` = 275400 B/us the GPU<->NVSwitch
    trunk rate (eta = 0.72 vs the 18 x 170 Gbps = 382500 B/us trunk physics)
    and ``t_min_switch`` = 4.10 us the fixed per-step cost (which also absorbs
    the D-dependent pipeline-fill residual -- at small D ns-3 runs faster than
    the structural alpha(N)=1/2/3 trunk load predicts, at large D slightly
    slower, see CALIBRATION.md). MAPE 12.65% over the 36-point
    N={4,8,16,32} x 1-256 MB ns-3 sweep (per-N 10.6/11.0/14.0/15.1%). ns-3
    anchored -- no H200 tree hardware ground truth exists in this repo.
    Accepts the same ``overrides`` as the ring profile (defaults to
    ``num_lanes=18``).
    """
    overrides.setdefault('num_lanes', 18)
    return make_h200_ring_theory(**overrides)


def make_h200_nvls_theory(**overrides):
    """H200 NVLS profile: hardware multicast, no credit flow control.

    Accepts the same ``overrides`` as the ring profile.
    """
    defaults = dict(
        packet_bw_bytes_per_us=177000,
        ring_bw_bytes_per_us=177000,
        tree_bw_per_level_bytes_per_us=275400,
        nvls_bw_bytes_per_us=535500,
        link_latency_us=0.4,
        credit_bdp_ring_packets=91.5,
        credit_bdp_tree_packets=35.5,
        spray_chunk_bytes=131072,
        bulk_chunk_bytes=8 * 1024 * 1024,
        num_lanes=18,
        t_min_switch_us=4.10,
        startup_us=23.0,
        nvls_startup_us=23.0,
    )
    defaults.update(overrides)
    return TheoryDerivedSurrogate(**defaults)


def make_h200_fullmesh_theory(**overrides):
    """H200 full-mesh profile: every GPU-pair has a dedicated NVLink bundle.

    ns-3 ``FullMeshAllReduce`` (2 phases: reduce-scatter + all-gather, each
    phase shipping the ``D/N`` chunk to all N-1 peers concurrently on
    dedicated per-pair bundles) is the ground truth this is calibrated to.
    Structurally it is NVLS WITHOUT the N-fold serialization -- each peer's
    bundle carries one chunk per phase, so ``T_step = (D/N)/fullmesh_bw``
    (no ``*N`` factor). ``fullmesh_bw`` is back-fit to the ns-3 sweep
    (``configs/calibration/fullmesh_ns3_sweep.py``); the model omits
    per-packet credit flow, VOQ arbitration and the inter-phase barrier that
    ns-3 models, so the least-squares fit STRADDLES ns-3 (slightly above at
    some fit points, below at others — a calibrated back-fit, not a bound;
    see CALIBRATION.md's "straddles ns-3" note). Scoped to the SIMPLE wire
    regime (chunk D/N >= 2
    MB); the LL/LL128 small-message regime is out of scope (same scope as the
    ring preset). Accepts the same ``overrides`` as the ring profile.
    """
    defaults = dict(
        packet_bw_bytes_per_us=177000,
        ring_bw_bytes_per_us=177000,
        tree_bw_per_level_bytes_per_us=275400,
        nvls_bw_bytes_per_us=535500,
        fullmesh_bw_bytes_per_us=449890,
        link_latency_us=0.4,
        credit_bdp_ring_packets=91.5,
        credit_bdp_tree_packets=35.5,
        spray_chunk_bytes=131072,
        bulk_chunk_bytes=8 * 1024 * 1024,
        num_lanes=18,
        t_min_switch_us=4.10,
        startup_us=15.0,
    )
    defaults.update(overrides)
    return TheoryDerivedSurrogate(**defaults)


def make_h200_alltoall_theory(**overrides):
    """H200 AlltoAll profile: full-exchange permutation over NVLink.

    Each GPU ships (N-1)/N of its data across N-1 partner phases (ring /
    collnetdirect) or ceil(log2 N) recursive-doubling rounds (tree), so the
    per-GPU volume is D*(N-1)/N -- the volume correction the DSE's
    ``bw_model_predict`` describes in comments but does not apply.

    The H200 AlltoAll calibration (``reproduction_checker`` H200_BEST_PARAMS)
    runs under the SIMPLE protocol with a 46 µs startup; AlltoAll contention is
    captured via the ``topology`` bisection cap rather than a separate
    bandwidth constant, so without a topology the model is an optimistic lower
    bound (the per-link wire rate). Accepts the same ``overrides`` as the ring
    profile.
    """
    overrides.setdefault('startup_us', 46.0)
    overrides.setdefault('num_lanes', 18)
    return make_h200_ring_theory(**overrides)


def make_h200_sendrecv_theory(**overrides):
    """H200 Send-Recv profile: single-hop point-to-point transfer.

    One rank sends the full payload to one peer: steps = 1, per-step = D, no
    pipeline credit pressure. The DSE's inline P2P model
    (``surrogate_pool_us``) is pure bandwidth (D/bw, no startup / propagation);
    this surrogate keeps the richer model -- kernel-launch startup +
    propagation + FEC / retransmission -- consistent with the other
    collectives. Accepts the same ``overrides`` as the ring profile.
    """
    overrides.setdefault('startup_us', 15.0)
    overrides.setdefault('num_lanes', 18)
    return make_h200_ring_theory(**overrides)


def make_h200_roce_theory(**overrides):
    """H200-class Ethernet RoCEv2 scale-out (So) fabric profile.

    Spec-derived, UNCALIBRATED lower bound (anchored to ns-3, not real HW):
    400GbE per-link unidirectional wire rate (50 GB/s = 50000 B/us), 8 NICs
    per node, ~1 us per-hop propagation (vs NVLink 0.4), ~1.5 us Ethernet
    switch pipeline (vs NVSwitch 4.10), ~40 us RDMA startup (vs NCCL 15), and
    0.95 wire efficiency (Eth+IP+UDP+IB header overhead -- mirrors the ns-3
    RoceProtocolModel's GetWireSize = data/0.95). The schedule bandwidths
    derive from the Ethernet wire rate the same way NVLink does (ring_bw =
    eta_ring * B_agg = 0.4 * 8 * 50000 = 160000 B/us). NVLS / tree bandwidths
    are inherited from the NVLink defaults but are unused on the So stage
    (NVLS is a scale-up in-network-compute concept; the So ring path is the
    hierarchical inter-node schedule). Accepts ``**overrides`` to tune any
    ``__init__`` param (e.g. ``link_latency_us=2.0`` for a longer-reach fabric).
    """
    defaults = dict(
        packet_bw_bytes_per_us=160000,
        ring_bw_bytes_per_us=160000,
        tree_bw_per_level_bytes_per_us=275400,
        nvls_bw_bytes_per_us=535500,
        link_latency_us=1.0,
        # Credit-UNCONSTRAINED: the ns-3 So (inter-node) path bypasses the
        # per-VC credit gate (SendBulkWireTransferSize's IsLocalRank guard --
        # scale-out traffic is governed by ECN/DCQCN or VOQ tail-drop + LLR,
        # not credits), so the So stage must not inherit the NVLink credit
        # BDP. float('inf') disables _credit_pressure on the ring path (the
        # tree BDP below is unused: the So schedule is the inter-node ring).
        credit_bdp_ring_packets=float('inf'),
        credit_bdp_tree_packets=float('inf'),
        spray_chunk_bytes=131072,
        bulk_chunk_bytes=8 * 1024 * 1024,
        num_lanes=8,
        t_min_switch_us=1.5,
        startup_us=40.0,
        wire_efficiency=0.95,
    )
    defaults.update(overrides)
    return TheoryDerivedSurrogate(**defaults)


def make_h200_ethernet_theory(**overrides):
    """H200-class plain (non-RoCE) Ethernet scale-out (So) fabric profile.

    The lossy counterpart to :func:`make_h200_roce_theory`: same 400GbE L2
    wire (``ring_bw`` is identical -- the physical link is the same), but a
    STORE-AND-FORWARD switch pipeline instead of RoCE's cut-through RDMA path.
    The store-and-forward latency dominates at small messages (the key
    physical difference vs RoCE); at large messages the serial term dominates
    and plain Ethernet is marginally faster (no RoCE IP/UDP/IB header
    overhead).

    Spec-derived, anchored to ns-3 (NOT real HW -- there is no perftest/blog
    ground truth for plain Ethernet, unlike RoCE's ib_write_lat):

    * ``wire_efficiency=1.0`` mirrors the ns-3 ``EthernetProtocolModel``,
      which locks ``WireEfficiency=1.0`` (ns-3 does not model Ethernet
      preamble/IFG framing, and plain L2 data carries no RoCE
      IP/UDP/IB headers). For ns-3 parity the surrogate must reproduce the
      ns-3 mean it is derived from, so 1.0 -- not RoCE's 0.95.
    * ``link_latency_us=3.0`` / ``t_min_switch_us=3.0`` model a commodity
      store-and-forward switch pipeline (cut-through RoCE uses 1.0 / 1.5).
    * ``startup_us=40``, ``ring_bw=160000``, ``num_lanes=8`` are shared with
      the RoCE profile (same wire, same NCCL inter-node startup).

    The surrogate is the UNCONGESTED MEAN. Lossy congestion behavior -- VOQ
    tail-drop, the switch-emitted NACK, and LLR/SACK retransmission -- is
    ns-3-only (the same split as RoCE: the surrogate gives the uncongested
    mean while DCQCN/PFC/per-QP-ceiling live in ns-3). Accepts ``**overrides``
    to tune any ``__init__`` param.
    """
    defaults = dict(
        packet_bw_bytes_per_us=160000,
        ring_bw_bytes_per_us=160000,
        tree_bw_per_level_bytes_per_us=275400,
        nvls_bw_bytes_per_us=535500,
        link_latency_us=3.0,
        # Credit-UNCONSTRAINED -- same reason as the RoCE profile (the ns-3 So
        # path bypasses VC credits; plain Ethernet So is governed by VOQ
        # tail-drop + LLR retransmit).
        credit_bdp_ring_packets=float('inf'),
        credit_bdp_tree_packets=float('inf'),
        spray_chunk_bytes=131072,
        bulk_chunk_bytes=8 * 1024 * 1024,
        num_lanes=8,
        t_min_switch_us=3.0,
        startup_us=40.0,
        wire_efficiency=1.0,
    )
    defaults.update(overrides)
    return TheoryDerivedSurrogate(**defaults)


def make_1024_optical_fattree_theory(**overrides):
    """Profile for the 1024-GPU, 600-Gbps fat-tree optical study.

    Accepts the same ``overrides`` as the ring profile.    """
    defaults = dict(
        packet_bw_bytes_per_us=75000,
        ring_bw_bytes_per_us=75000,
        tree_bw_per_level_bytes_per_us=20725,
        nvls_bw_bytes_per_us=75000,
        link_latency_us=0.4,
        credit_bdp_ring_packets=44.5,
        credit_bdp_tree_packets=44.5,
        spray_chunk_bytes=131072,
        bulk_chunk_bytes=8 * 1024 * 1024,
        num_lanes=1,
        t_min_switch_us=1.1,
        startup_us=65.0,
    )
    defaults.update(overrides)
    return TheoryDerivedSurrogate(**defaults)


def make_connectx7_p2p_perftest_theory(**overrides):
    """ConnectX-7 400G RoCEv2 P2P perftest-calibrated profile (raw RDMA layer).

    Calibrated to the ib_write_lat single-write latency curve from a
    third-party perftest blog (NVIDIA ConnectX-7 400G, RoCEv2 GID index 3,
    MTU 4096, RC, TX depth 128). Provenance + the fit live in
    ``configs/calibration/roce_p2p_calib.py`` / ``.json``; the data source is
    a third-party engineering blog, NOT the project's A800 hardware, so this
    is a soft externally-anchored calibration of the raw-RDMA scalar
    constants, not a collective-fidelity verdict.

    This models the RAW RDMA layer (NIC doorbell + same-switch propagation +
    write completion) which sits BELOW the NCCL collective layer: there is no
    NCCL kernel-launch startup, no collective algorithm. ``predict(D, 2,
    'ring', credits=128, collective='sendrecv')`` reproduces ib_write_lat
    within ~3% MAPE (the single-write curve is sublinear in NIC processing;
    the linear surrogate fits the 2B/64B/64KB endpoints closely and the 1KB
    transition within ~8%).

    Calibrated constants (ib_write_lat fit): per-message fixed latency 1.45 us
    (doorbell + completion 1.05 us + same-switch propagation 0.4 us), single-
    write effective serialization 192 Gbps (24000 B/us -- NIC-processing-
    limited, below the 400G line rate; already an effective rate, so
    wire_efficiency=1.0 here), single QP (num_lanes=1). Separately extracted
    (NOT expressed by this latency model): the per-QP sustained throughput
    ceiling ~49 Gbps and the 0.988 wire efficiency (8-QP saturation 0.9875)
    are throughput/aggregate concepts -- the 49G ceiling is applied in ns-3
    via the opt-in PerQpCeilingGbps attribute, not here.
    """
    defaults = dict(
        packet_bw_bytes_per_us=24000,    # 192 Gbps single-write effective rate
        ring_bw_bytes_per_us=24000,
        tree_bw_per_level_bytes_per_us=24000,
        nvls_bw_bytes_per_us=24000,
        link_latency_us=0.4,            # same-switch propagation
        startup_us=1.05,                # NIC doorbell + write completion
        wire_efficiency=1.0,            # 192 Gbps is already an effective rate
        num_lanes=1,                    # single QP / single flow
        t_min_switch_us=0.0,
        bulk_chunk_bytes=8 * 1024 * 1024,
        spray_chunk_bytes=131072,
    )
    defaults.update(overrides)
    return TheoryDerivedSurrogate(**defaults)


def make_surrogate(algo, **overrides):
    """Factory by algorithm name; passes ``overrides`` to the chosen profile."""
    if algo in ('ring', 'rin'):
        return make_h200_ring_theory(**overrides)
    elif algo in ('tree', 'tre'):
        return make_h200_tree_theory(**overrides)
    elif algo in ('nvls', 'nvl'):
        return make_h200_nvls_theory(**overrides)
    elif algo in ('fullmesh', 'fm'):
        return make_h200_fullmesh_theory(**overrides)
    else:
        return make_h200_ring_theory(**overrides)


def make_surrogate_from_wire(b_link_bytes_per_us, num_lanes, algo,
                             eta_ring=0.40, eta_tree=0.61, eta_nvls=1.20,
                             eta_fullmesh=1.0, **overrides):
    """Derive every schedule bandwidth from ONE NVLink wire-rate input.

    A single physical base -- the per-GPU NVLink aggregate
    ``B_agg = num_lanes * b_link`` -- plus three physically-motivated
    efficiency factors reproduces the calibrated H200 bandwidths within 2%
    with no per-bandwidth calibration:

      ring_bw = packet_bw = eta_ring * B_agg
                           ~0.40 (AllReduce algorithm-bandwidth realization;
                                  bus bw 2(N-1)/N * alBw ~= 0.69 * B_agg,
                                  in NCCL's typical 60-80% band)
      tree_bw = eta_tree * B_agg
                           ~0.61 (effective GPU<->NVSwitch trunk realization:
                                  the ns-3 back-fit 275400 B/us over the
                                  382500 B/us trunk = eta 0.72 vs the
                                  effective 170 Gbps/lane rate, i.e. 0.61 of
                                  the 25000 B/us physical wire aggregate)
      nvls_bw = eta_nvls * B_agg
                           ~1.20 (NVSwitch hardware multicast amplification)
      fullmesh_bw = eta_fullmesh * B_agg
                           ~1.0 (dedicated per-peer bundle uses the full
                                  aggregate; full-mesh has no N-fold switch
                                  serialization, unlike nvls)

    For H200 (b_link=25000 B/us, num_lanes=18, B_agg=450000) this yields
    ring/packet=180000 (calibrated 177000, +1.7%), tree=274500 (ns-3-calibrated
    275400, -0.3%), nvls=540000 (calibrated 535500, +0.8%), fullmesh=450000
    (ns-3-calibrated, the dedicated 18-lane-per-pair bundle within the fit
    band). Pass ``ring_bw_bytes_per_us=`` etc.
    in ``overrides`` to override any derived value. The calibrated
    ``make_h200_*`` factories still ship the exact measured values; this is
    the single-parameter entry point for new platforms where only the
    per-link wire rate is known.
    """
    b_agg = b_link_bytes_per_us * num_lanes
    derived = dict(
        packet_bw_bytes_per_us=eta_ring * b_agg,
        ring_bw_bytes_per_us=eta_ring * b_agg,
        tree_bw_per_level_bytes_per_us=eta_tree * b_agg,
        nvls_bw_bytes_per_us=eta_nvls * b_agg,
        fullmesh_bw_bytes_per_us=eta_fullmesh * b_agg,
        num_lanes=num_lanes,
    )
    derived.update(overrides)
    return make_surrogate(algo, **derived)


def make_topology(bisection_gbps=None, hop_count=1.0):
    """Build a ``Topology`` from a bisection bandwidth in GB/s.

    1 GB/s = 1000 bytes/µs (the surrogate's bandwidth units). ``bisection_gbps``
    is the fabric's bisection bandwidth; pass ``None`` (default) for a
    non-blocking fabric whose bisection never caps the schedule rate.
    ``hop_count`` is the fabric path length in link hops (1 = single switch).
    """
    bsec = bisection_gbps * 1000.0 if bisection_gbps is not None else float('inf')
    return Topology(bisection_bw_bytes_per_us=bsec, hop_count=hop_count)


# ---------------------------------------------------------------------------
# Topology from a family NAME (formulas copied from scripts/topo_grammar.py).
#
# These replicate TopoSpec._bisection_bw / _default_links_per_gpu and the
# FAMILIES hop functions verbatim, so a caller gives a topology name plus
# physical numbers and gets a Topology instead of hand-computing the two
# values for make_topology().  The hop_count reproduces the representative
# per-step hop of the EMBEDDED collective (ring follows a locality-preserving
# order so consecutive ranks are physically local; leaf-tiers add one 2-hop
# jump per leaf transition).
#
# UNIT CONVENTION: pass ``per_link_gbps`` as the per-link UNIDIRECTIONAL wire
# rate in GB/s, i.e. b_link_bytes_per_us / 1000 (25 for H200 NVLink4).  The DSE
# grammar instead keys per-link BW off a link_tech table whose NVLink entry is
# the bidirectional 100 GB/s sheet figure; the two are not interchangeable
# because here the bisection caps the theory surrogate's OWN algo bandwidth
# (built from b_link), so the units must agree with b_link, not the grammar.
# ---------------------------------------------------------------------------

_TOPO_FAMILIES = (
    "ring", "fullmesh", "hypercube", "3d_torus", "mesh2d", "2dfullmesh",
    "switched", "nvl72", "multiplane", "leafspine", "3levelhier", "fattree",
    "railfattree", "dragonflyplus", "2dfullmeshclos",
    # Scale-out (inter-node) Ethernet RoCEv2 fabrics. N is the NODE count and
    # links_per_gpu is NICs-per-node; per_link_gbps is the Ethernet wire rate
    # (50 = 400GbE unidirectional). These feed the So stage of the hierarchical
    # surrogate (see HierarchicalSurrogate); the Su stage uses the families
    # above with N = GPUs-per-node.
    "so_fullmesh", "so_spineleaf", "so_fattree",
)

# Non-blocking NVSwitch fabrics: bisection never caps the schedule rate, so
# the result is the ideal fabric (byte-identical to topology=None).
_NONBLOCKING_FAMILIES = ("switched", "nvl72", "multiplane")


def _default_links_per_gpu(family, N, **p):
    """Per-family default links-per-GPU (copy of TopoSpec._default_links_per_gpu)."""
    if family == "hypercube":
        return p.get("hypercube_dims") or 7
    if family == "3d_torus":
        return 6
    if family == "mesh2d":
        return 4
    if family == "2dfullmesh":
        return 4
    if family == "ring":
        return 2
    if family == "fullmesh":
        return N - 1
    if family == "switched":
        return 18
    if family == "nvl72":
        return p.get("num_planes") or 18
    if family == "multiplane":
        return (p.get("num_planes") or 4) * 4
    if family == "leafspine":
        return 8
    if family == "3levelhier":
        return 8
    if family == "fattree":
        return p.get("radix") or 8
    if family == "railfattree":
        return 1
    if family == "dragonflyplus":
        return 8
    if family == "2dfullmeshclos":
        return 8
    # Scale-out Ethernet fabrics: 8 NICs per node (H200-class dual-port 400GbE
    # NICs across 8 rails). links_per_gpu here is NICs-per-node; N is node count.
    if family in ("so_fullmesh", "so_spineleaf", "so_fattree"):
        return 8
    return 1


def _bisection_bw_gbps(family, N, links_per_gpu, per_link_gbps, **p):
    """Per-family bisection bandwidth in GB/s (copy of TopoSpec._bisection_bw)."""
    lpg = links_per_gpu
    if family in ("fullmesh", "switched", "nvl72", "railfattree"):
        return N * lpg * per_link_gbps / 2          # full bisection
    if family == "hypercube":
        return N * per_link_gbps / 2                 # 1-dim cut
    if family in ("3d_torus", "mesh2d", "2dfullmesh"):
        dims = [d for d in (p.get("dims") or ()) if d > 0] or [1]
        cut_gpus = N // min(dims)                    # cut across smallest dim
        return cut_gpus * per_link_gbps / 2
    if family == "ring":
        return 2 * per_link_gbps                     # cut = 2 links
    # Scale-out Ethernet: so_fullmesh is non-blocking (full bisection, the
    # per-node NIC is the bottleneck, not the fabric); so_spineleaf / so_fattree
    # are oversubscribed inter-tier (8:1 cut, like leafspine).
    if family == "so_fullmesh":
        return N * lpg * per_link_gbps / 2          # full bisection (non-blocking)
    if family in ("so_spineleaf", "so_fattree"):
        return N * lpg * per_link_gbps / 8          # inter-tier oversubscribed cut
    # leafspine / 3levelhier / fattree / dragonflyplus / 2dfullmeshclos
    return N * lpg * per_link_gbps / 8               # bisection ~ inter-tier links


def _leaftier_hop(N, leaf):
    """Copy of topo_grammar._leaftier_hop: ring is NodeId-sorted so same-leaf
    ranks are contiguous (1 hop); one 2-hop jump per leaf transition."""
    steps = N - 1
    if steps <= 0:
        return 1.0
    inter = leaf
    avg = ((steps - inter) * 1 + inter * 2) / steps
    return max(1.0, avg)


def _rail_fattree_defaults(N, rails=8, nodes_per_leaf=32):
    """Copy of topo_grammar._rail_fattree_defaults: published switch counts."""
    nodes = math.ceil(N / rails)
    total_leaves = rails * math.ceil(nodes / nodes_per_leaf)
    if total_leaves <= 64:
        spines = next(candidate
                      for candidate in (4, 8, 16, 32)
                      if candidate >= math.ceil(total_leaves / 2))
        return spines, 32 // spines, 0
    return total_leaves, 1, total_leaves // 2


def _rail_fattree_shape(N, rails, nodes_per_leaf, rail_num_core,
                        num_spine, rail_links_per_spine):
    """Copy of topo_grammar._rail_fattree_shape: leaves/spines/cores/links."""
    nodes = math.ceil(N / rails)
    leaves_per_rail = math.ceil(nodes / nodes_per_leaf)
    total_leaves = rails * leaves_per_rail
    default_spines, default_links, default_cores = _rail_fattree_defaults(
        N, rails, nodes_per_leaf)
    spines = num_spine or default_spines
    cores = rail_num_core or default_cores
    links_per_spine = rail_links_per_spine or default_links
    return leaves_per_rail, total_leaves, spines, cores, links_per_spine


def _railfattree_hop(N, **p):
    """Copy of topo_grammar._railfattree_hop: count switch hops along the
    rail-major collective order (1 within a leaf, 2 within a group, 3 across
    the core tier)."""
    rails = p.get("rail_count") or 8
    enclosures = N // rails
    order = [node * rails + rail
             for rail in range(rails)
             for node in range(enclosures)]
    nodes_per_leaf = p.get("rail_nodes_per_leaf") or 32
    leaves_per_rail, _, _, cores, _ = _rail_fattree_shape(
        N, rails, nodes_per_leaf, p.get("rail_num_core"),
        p.get("num_spine"), p.get("rail_links_per_spine"))

    def leaf(rank):
        node, rail = divmod(rank, rails)
        return rail * leaves_per_rail + node // nodes_per_leaf

    hops = []
    for source, destination in zip(order, order[1:]):
        source_leaf = leaf(source)
        destination_leaf = leaf(destination)
        if source_leaf == destination_leaf:
            hops.append(1.0)
        elif cores and source_leaf // 32 != destination_leaf // 32:
            hops.append(3.0)
        else:
            hops.append(2.0)
    return sum(hops) / len(hops) if hops else 1.0


def _hop_count(family, N, **p):
    """Representative per-step hop of the embedded collective
    (copy of the FAMILIES hop functions in scripts/topo_grammar.py)."""
    if family in ("ring", "fullmesh", "hypercube", "3d_torus",
                  "mesh2d", "2dfullmesh"):
        return 1.0                                     # _direct_hop
    # Scale-out Ethernet: so_fullmesh (fullmesh-of-switches) is non-blocking so
    # its 2-ToR traversal is the lower bound (hop=1, no extra propagation);
    # so_spineleaf adds 1 inter-switch hop (leaf->spine->leaf = 2), so_fattree
    # adds 2 (3-tier). These are the So-stage representative per-step hops.
    if family == "so_fullmesh":
        return 1.0
    if family == "so_spineleaf":
        return 2.0
    if family == "so_fattree":
        return 3.0
    if family in _NONBLOCKING_FAMILIES:
        return 1.0                                     # _switched_hop
    if family in ("leafspine", "3levelhier", "fattree",
                  "dragonflyplus", "2dfullmeshclos"):
        return _leaftier_hop(N, p.get("num_leaf") or 1)
    if family == "railfattree":
        return _railfattree_hop(N, **p)
    return 1.0


def make_topology_from_family(family, N, links_per_gpu=None,
                              per_link_gbps=None, **params):
    """Build a ``Topology`` from a fabric family NAME plus physical numbers.

    Encodes the per-family bisection-bandwidth and hop-count formulas of the
    DSE topology grammar (``scripts/topo_grammar.py::TopoSpec``) so a caller
    gives a topology name and physical inputs and gets back a ``Topology``,
    instead of hand-computing the two numbers for :func:`make_topology`.

    Parameters
    ----------
    family : str
        One of: ``ring``, ``fullmesh``, ``hypercube``, ``3d_torus``,
        ``mesh2d``, ``2dfullmesh``, ``switched``, ``nvl72``, ``multiplane``,
        ``leafspine``, ``3levelhier``, ``fattree``, ``railfattree``,
        ``dragonflyplus``, ``2dfullmeshclos``, or a scale-out Ethernet family
        (``so_fullmesh`` / ``so_spineleaf`` / ``so_fattree`` -- here ``N`` is
        the NODE count, ``links_per_gpu`` is NICs-per-node, and
        ``per_link_gbps`` is the Ethernet wire rate, 50 = 400GbE).
    N : int
        Participating GPU count.
    links_per_gpu : int, optional
        Physical links per GPU. Defaults per family (ring=2, switched=18,
        fullmesh=N-1, ...); pass to override.
    per_link_gbps : float, optional
        Per-link UNIDIRECTIONAL wire rate in GB/s, i.e.
        ``b_link_bytes_per_us / 1000`` (e.g. 25 for H200 NVLink4). Required
        for bisection-bound families (ring/torus/mesh/leaf-tiers); optional
        for non-blocking NVSwitch fabrics (``switched``/``nvl72``/
        ``multiplane``), whose bisection never caps the schedule rate.
    **params
        Family-specific builder params (all optional, defaulted): ``dims``
        (tuple, torus/mesh/2dfullmesh), ``hypercube_dims``, ``radix``
        (fattree), ``num_leaf``/``num_spine``/``num_mid`` (leafspine/hier3/
        dragonfly), ``num_planes`` (nvl72/multiplane), ``num_racks``/
        ``rack_rows`` (2dfullmeshclos), ``rail_count``/``rail_nodes_per_leaf``/
        ``rail_links_per_spine``/``rail_num_core`` (railfattree).

    Returns
    -------
    Topology
        ``bisection_bw_bytes_per_us`` = family bisection (GB/s x1000) capping
        the schedule rate; ``hop_count`` = representative per-step hop. For a
        non-blocking NVSwitch fabric the bisection is effectively infinite and
        ``hop_count`` = 1, so the result is byte-identical to ``topology=None``.

    Notes
    -----
    The bisection formula and hop functions are copied verbatim from
    ``topo_grammar.TopoSpec``; the only divergence is the per-link unit
    convention (unidirectional GB/s here vs the grammar's link_tech table),
    kept so the cap is consistent with the surrogate's ``b_link``-derived
    bandwidth. ``N < 2`` is degenerate (no collective bisection) and returns
    the ideal fabric.
    """
    if family not in _TOPO_FAMILIES:
        raise ValueError(
            f"unknown topology family {family!r}; "
            f"expected one of {_TOPO_FAMILIES}")
    if N < 2:
        return make_topology()                         # degenerate: no fabric
    if per_link_gbps is None and family not in _NONBLOCKING_FAMILIES:
        raise ValueError(
            f"per_link_gbps is required for {family!r} "
            "(per-link unidirectional GB/s, e.g. 25 for H200 NVLink4 "
            "= b_link_bytes_per_us / 1000)")
    if family in _NONBLOCKING_FAMILIES:
        # A non-blocking NVSwitch fabric's bisection never caps the schedule
        # rate (both guides promise this), so per_link_gbps — supplied or
        # omitted — never enters the result. The old code fell through to
        # _bisection_bw_gbps when the rate was supplied, which routed
        # multiplane to the leaf-tier oversubscription formula (N*lpg*rate/8)
        # and capped a fabric that is documented as uncapped.
        return make_topology(bisection_gbps=None, hop_count=1.0)
    lpg = links_per_gpu or _default_links_per_gpu(family, N, **params)
    bisection_gbps = _bisection_bw_gbps(family, N, lpg, per_link_gbps, **params)
    hop = _hop_count(family, N, **params)
    return make_topology(bisection_gbps=bisection_gbps, hop_count=hop)


# ---------------------------------------------------------------------------
# Hierarchical (scale-up Su + scale-out So) collective surrogate.
#
# Once a job spans more than one node, NCCL runs the collective hierarchically:
# each node does the collective locally over NVLink (Su), one representative
# per node does it across nodes over Ethernet RoCEv2 (So), then the result is
# redistributed locally (Su). This class composes a Su + a So sub-collective
# into that hierarchical latency. The So term is an UNCALIBRATED LOWER BOUND
# anchored to ns-3 (spec-derived RoCEv2 params, not real-HW calibration) --
# like the NVLS band, the hierarchical latency sits below the ns-3 packet-level
# simulation (the analytical bus-bandwidth model omits per-packet credit flow,
# VOQ arbitration, PFC/MTU overhead, and per-phase barrier syncs that ns-3
# models).
# ---------------------------------------------------------------------------

class HierarchicalSurrogate:
    """Two-stage (Su -> So) hierarchical collective latency surrogate.

    The Su stages run on ``su`` (NVLink bandwidth / latency / startup); the So
    stages run on ``so`` (Ethernet bandwidth / latency / startup /
    wire-efficiency) under ``so_topology`` (the So fabric's bisection cap +
    multi-hop propagation). Phase decomposition (each phase is a single ring
    pass; ``predict`` with ``phase='rs'`` / ``'ag'`` halves the step count so
    the serial volume is one bus-factor and the fixed per-step latency is that
    of a single pass):

      allreduce (4-phase): L = L_su(rs) + L_so(rs) + L_so(ag) + L_su(ag)
      allgather (2-phase): L = L_su(ag) + L_so(ag)
      alltoall  (2-phase): L = L_su(alltoall) + L_so(alltoall)
      sendrecv          : L = L_so(sendrecv)  (single inter-node hop, no Su stage)

    Per-phase data sizes follow the true hierarchical algorithm, not all full
    ``data_bytes``: the allreduce inter-node RS/AG stages move only the
    per-node reduced chunk ``D/n_su`` (after intra-node reduce-scatter each
    node holds ``D/n_su``, matching ``ns-3 hierarchical-allreduce.cc``); the
    allgather first intra-node stage gathers the per-node share ``D/n_so``
    (each GPU starts with ``D/N``) while its inter-node stage allgathers the
    FULL ``D`` across the n_so same-local-rank groups (each GPU exchanges the
    node buffer D/n_so with each of its n_so-1 peers: per-GPU So volume
    D*(n_so-1)/n_so) — two phases, matching ``ns-3 hierarchical-allgather.cc``
    whose ``Complete()`` fires at the end of ``INTER_ALLGATHER`` (no third
    local broadcast); alltoall ships ``D*(n_su-1)/n_su`` on the Su stage and
    ``D*(n_so-1)/n_so`` on the So stage (the hierarchical shuffle routes each
    GPU's (N-1)/N share through its node: the local share to local peers, the
    remote share via the node's exit GPU), matching
    ``ns-3 hierarchical-alltoall.cc``.
    stages. Each stage's bus-factor volume ``D_phase*(N-1)/N`` then comes from
    the ring per-step model. ``collective`` is a REQUIRED keyword-only argument
    to ``predict`` (never inferred from ``algo``), consistent with
    :class:`TheoryDerivedSurrogate`.
    """

    _VALID = ('allreduce', 'allgather', 'alltoall', 'sendrecv')

    def __init__(self, su, so, gpus_per_node, num_nodes, so_topology=None):
        self.su = su
        self.so = so
        self.n_su = gpus_per_node
        self.n_so = num_nodes
        self.so_topology = so_topology

    def _phase(self, data_bytes, n, fabric, algo, phase, credits, topology,
               collective, **kw):
        """One single-pass ring sub-collective (rs/ag) on ``fabric`` for ``n``
        participants under ``topology`` (None = Su / ideal fabric)."""
        return fabric.predict(
            data_bytes, n, algo, credits, collective=collective,
            phase=phase, topology=topology, **kw)

    def predict(self, data_bytes, algo, credits, *, collective, **kw):
        """Predict hierarchical collective latency (µs).

        ``collective`` (required) selects the phase decomposition. Other
        kwargs (``ber``, ``fec_enabled``, ``credits`` already positional, ...)
        forward to each stage's :meth:`TheoryDerivedSurrogate.predict`.
        """
        if collective not in self._VALID:
            raise ValueError(
                "collective must be one of %s, got %r" % (self._VALID, collective))
        if collective == 'sendrecv':
            # A single inter-node point-to-point transfer has no Su stage.
            return self.so.predict(
                data_bytes, self.n_so, algo, credits, collective='sendrecv',
                topology=self.so_topology, **kw)
        if collective == 'alltoall':
            # 2-phase, matching ns-3 hierarchical-alltoall.cc: every GPU
            # starts with the per-GPU chunk D/(n_su*n_so) (dataSize/numGpus).
            # Su phase: each GPU sends its chunk to its n_su-1 local peers —
            # per-GPU volume D*(n_su-1)/(n_su*n_so), which is what an alltoall
            # of data D/n_so over n_su GPUs models. So phase: each GPU sends
            # its chunk to ALL GPUs on the other nodes (n_so-1 nodes x n_su
            # GPUs) — per-GPU volume D*(n_so-1)/n_so, i.e. a full alltoall of
            # the FULL D across n_so "super-participants" (each GPU's egress
            # is its node's exit trunk).
            su_chunk = data_bytes // self.n_so
            return (self.su.predict(su_chunk, self.n_su, algo, credits,
                                   collective='alltoall', **kw)
                    + self.so.predict(data_bytes, self.n_so, algo, credits,
                                      collective='alltoall',
                                      topology=self.so_topology, **kw))
        if collective == 'allreduce':
            # 4-phase: local RS -> inter-node RS -> inter-node AG -> local AG.
            # Su phases (rs/ag) move the full D; the inter-node So phases move
            # only the per-node reduced chunk D/n_su (after su_rs each node
            # holds D/n_su), matching ns-3 hierarchical-allreduce.cc.
            so_chunk = data_bytes // self.n_su
            return (self._phase(data_bytes, self.n_su, self.su, algo, 'rs',
                                credits, None, 'allreduce', **kw)
                    + self._phase(so_chunk, self.n_so, self.so, algo, 'rs',
                                  credits, self.so_topology, 'allreduce', **kw)
                    + self._phase(so_chunk, self.n_so, self.so, algo, 'ag',
                                  credits, self.so_topology, 'allreduce', **kw)
                    + self._phase(data_bytes, self.n_su, self.su, algo, 'ag',
                                  credits, None, 'allreduce', **kw))
        # allgather (2-phase): local AG -> inter-node AG, matching
        # ns-3 hierarchical-allgather.cc (Complete() fires at the end of
        # INTER_ALLGATHER). Phase 1 gathers the per-node share D/n_so
        # intra-node (each GPU starts with D/N; the ring model's D for that
        # stage is the gathered node buffer D/n_so). Phase 2 is an allgather
        # across the n_so same-local-rank groups where the gathered buffer is
        # the FULL D: each GPU exchanges the node buffer D/n_so with each of
        # its n_so-1 remote peers, per-GPU So volume D*(n_so-1)/n_so — the
        # ring model must get the FULL data_bytes here, not the node share
        # (passing node_share would divide by n_so twice). No third
        # local-broadcast phase.
        node_share = data_bytes // self.n_so
        return (self._phase(node_share, self.n_su, self.su, algo, 'ag',
                            credits, None, 'allgather', **kw)
                + self._phase(data_bytes, self.n_so, self.so, algo, 'ag',
                              credits, self.so_topology, 'allgather', **kw))


def make_h200_hierarchical_theory(gpus_per_node=8, num_nodes=2,
                                 so_topology_family='so_fullmesh',
                                 so_per_link_gbps=50.0, so_links_per_gpu=8,
                                 so_factory=make_h200_roce_theory,
                                 **so_overrides):
    """H200-class hierarchical (Su NVLink + So Ethernet RoCEv2) profile.

    Returns a :class:`HierarchicalSurrogate` combining a scale-up H200 NVLink
    surrogate (``make_h200_ring_theory``) with a scale-out Ethernet RoCEv2
    surrogate (``make_h200_roce_theory``) under a So topology
    (``so_fullmesh`` non-blocking lower bound by default; ``so_spineleaf`` /
    ``so_fattree`` for oversubscribed fabrics whose bisection caps the So
    schedule rate). The So term is an uncalibrated lower bound anchored to
    ns-3. Pass ``so_factory=make_h200_ethernet_theory`` to swap the So stage
    to a plain (non-RoCE) lossy Ethernet fabric (same wire, store-and-forward
    switch latency); the default remains RoCEv2.

    Parameters
    ----------
    gpus_per_node : int
        Su (intra-node) rank count = GPUs per node.
    num_nodes : int
        So (inter-node) rank count = node count.
    so_topology_family : str
        So fabric family (``so_fullmesh`` / ``so_spineleaf`` / ``so_fattree``).
    so_per_link_gbps : float
        So per-link unidirectional wire rate (50 = 400GbE).
    so_links_per_gpu : int
        NICs per node (the So ``links_per_gpu``; N is node count).
    so_factory : callable, optional
        Factory building the So (scale-out) profile (default
        :func:`make_h200_roce_theory`). Pass :func:`make_h200_ethernet_theory`
        to model a plain (non-RoCE) lossy Ethernet So fabric instead of RoCE.
    **so_overrides
        Forwarded to ``so_factory`` to tune So startup / latency /
        wire-efficiency / bandwidth.

    ``collective`` stays required at ``predict`` time (pass it explicitly --
    it is not inferred from ``algo``); the decomposition is selected by the
    collective, not the algorithm.
    """
    su = make_h200_ring_theory()
    # The So hardware args must shape the So SURROGATE, not just the topology
    # cap: the RoCE profile's bandwidth constants are the 8-NIC 400GbE
    # aggregate (ring_bw = 160000 B/us), so a different NIC count or wire rate
    # rescales them. Without this, 1 vs 8 NICs predicted identically as long
    # as the topology bisection did not bind.
    nic_scale = so_links_per_gpu / 8.0
    wire_scale = so_per_link_gbps / 50.0
    so_args = dict(so_overrides)
    so_args.setdefault('num_lanes', so_links_per_gpu)
    if (nic_scale * wire_scale) != 1.0 and 'ring_bw_bytes_per_us' not in so_args:
        so_args['ring_bw_bytes_per_us'] = 160000 * nic_scale * wire_scale
        so_args.setdefault('packet_bw_bytes_per_us', 160000 * nic_scale * wire_scale)
    so = so_factory(**so_args)
    so_topo = make_topology_from_family(
        so_topology_family, num_nodes,
        links_per_gpu=so_links_per_gpu, per_link_gbps=so_per_link_gbps)
    return HierarchicalSurrogate(su, so, gpus_per_node, num_nodes, so_topo)
