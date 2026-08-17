#!/usr/bin/env python3
"""
Analytical Surrogate Model for Supernode Fabric Latency Prediction.

Regression-calibrated piecewise model with FEC/retry/memory amplification.
Fits alpha (startup/latency overhead) and beta (BW efficiency) from ns-3 data,
then applies amplification factors for optical reliability and memory disaggregation.

Target: predictions within 10% of ns-3 for calibrated configurations.
"""

import json
import math
import numpy as np
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple


class QueuingTheoryEstimator:
    """Queuing-theoretic latency estimation using M/G/1, Kingman, and M/D/1.

    Provides mean waiting time estimates from established queuing theory results.
    P95/P99 tail estimation uses empirical calibration against ns-3 bootstrap
    results (not analytical derivation from PK/Kingman, which only give mean).

    Key formulas:
    - Pollaczek-Khinchine (M/G/1 mean wait): W_q = (lambda * E[S^2]) / (2 * (1 - rho))
      where rho = lambda * E[S]
    - Kingman (G/G/1 heavy-traffic approximation): W_q ≈ E[S] * rho/(1-rho) * (c_a^2 + c_s^2)/2
    - M/D/1 (deterministic service): W_q = rho/(2*(1-rho)) * E[S]
    """

    def __init__(self):
        # Arrival/service coefficient of variation (calibrated from ns-3 data)
        self.c_a_squared = 1.0   # Arrival CV^2 (Poisson default = 1.0)
        self.c_s_squared = 0.0   # Service CV^2 (deterministic default = 0.0)

        # Empirical tail calibration factors: P95 = P50 * alpha_p95, P99 = P50 * alpha_p99
        # These are fitted from ns-3 bootstrap P50/P95/P99 data per config group.
        self.alpha_p95 = 1.02    # Default: near-zero variance baseline
        self.alpha_p99 = 1.05    # Default: near-zero variance baseline
        self._tail_calibrated = False
        self._tail_calibration_data = {}  # keyed by (fec_mode, ber_level, size_group)

    def mg1_mean_wait(self, arrival_rate: float, mean_service: float,
                      second_service_moment: float) -> float:
        """M/G/1 mean waiting time via Pollaczek-Khinchine formula.

        W_q = (lambda * E[S^2]) / (2 * (1 - rho))
        where rho = lambda * E[S]

        Returns mean waiting time in same units as service time input.
        """
        rho = arrival_rate * mean_service
        if rho <= 0:
            return 0.0
        if rho >= 1.0:
            return float('inf')
        return (arrival_rate * second_service_moment) / (2.0 * (1.0 - rho))

    def kingman_bound(self, arrival_rate: float, mean_service: float) -> float:
        """Kingman's heavy-traffic approximation for mean waiting time in G/G/1.

        W_q ≈ E[S] * rho/(1-rho) * (c_a^2 + c_s^2)/2

        Heavy-traffic approximation; useful as sanity check but not a
        tight bound at low utilization.
        """
        rho = arrival_rate * mean_service
        if rho <= 0:
            return 0.0
        if rho >= 1.0:
            return float('inf')
        return mean_service * rho / (1.0 - rho) * (self.c_a_squared + self.c_s_squared) / 2.0

    def md1_mean_wait(self, arrival_rate: float, mean_service: float) -> float:
        """M/D/1 mean waiting time (exact result for deterministic service).

        W_q = rho/(2*(1-rho)) * E[S]

        Used for memory service queue (deterministic service time =
        accessLatency + size/bandwidth).
        """
        rho = arrival_rate * mean_service
        if rho <= 0:
            return 0.0
        if rho >= 1.0:
            return float('inf')
        return rho / (2.0 * (1.0 - rho)) * mean_service

    def wfq_effective_bw(self, total_bw: float, class_weight: float,
                          num_active_classes: int) -> float:
        """WFQ effective bandwidth for a traffic class.

        If only one class active: full bandwidth.
        If multiple classes: weight * total_bandwidth.
        """
        if num_active_classes <= 1:
            return total_bw
        return class_weight * total_bw

    def collective_queueing_delay(self, data_size_bytes: int,
                                   aggregate_bw_gbps: float,
                                   collective_weight: float,
                                   num_active_classes: int,
                                   remote_fraction: float = 0.0) -> float:
        """Compute collective traffic queuing delay contribution.

        When remote memory traffic is active, WFQ reduces collective's
        effective BW. Delay ≈ base_transfer * (1/weight - 1) * remote_frac.
        """
        if remote_fraction <= 0 or num_active_classes <= 1:
            return 0.0
        if collective_weight <= 0 or collective_weight >= 1.0:
            return 0.0

        bw_bytes_per_us = aggregate_bw_gbps / 8 * 1000
        seg_size = data_size_bytes / 4  # approximate segment
        base_transfer_us = seg_size / bw_bytes_per_us

        # WFQ contention: collective loses (1-weight) fraction of BW
        # to memory traffic proportional to remote fraction
        return base_transfer_us * (1.0 / collective_weight - 1.0) * remote_fraction

    def memory_queueing_delay(self, data_size_bytes: int,
                                remote_fraction: float,
                                mem_bw_gbps: float,
                                mem_access_latency_ns: float) -> float:
        """Compute memory service queuing delay via M/D/1 model.

        Memory service has deterministic service time (access_latency + size/BW),
        so M/D/1 exact formula applies.
        """
        if remote_fraction <= 0 or mem_bw_gbps <= 0:
            return 0.0

        # bandwidthGBps config value is already in GB/s, convert to bytes/s
        mem_bw_bytes_per_s = mem_bw_gbps * 1e9
        remote_bytes = data_size_bytes * remote_fraction

        # Deterministic service time for a memory request
        mean_service_s = (mem_access_latency_ns * 1e-9 +
                          remote_bytes / mem_bw_bytes_per_s)

        # Arrival rate: assume moderate memory request intensity
        rho = min(0.6, remote_fraction * 0.8)
        if rho <= 0 or mean_service_s <= 0:
            return 0.0
        arrival_rate = rho / mean_service_s

        wait_s = self.md1_mean_wait(arrival_rate, mean_service_s)
        return wait_s * 1e6  # seconds to µs

    def estimate_tail_percentiles(self, p50: float,
                                    p_pkt_error: float,
                                    fec_mode: str = "none",
                                    ber_level: str = "0") -> Tuple[float, float]:
        """Estimate P95/P99 from P50 using calibrated tail scaling factors.

        Does NOT claim analytical tail derivation from PK/Kingman.
        Uses empirical calibration factors fitted from ns-3 bootstrap results.
        Falls back to conservative heuristics when calibration data is absent.

        Args:
            p50: P50 latency in µs
            p_pkt_error: Packet/segment error probability
            fec_mode: "fec", "retry", or "none"
            ber_level: BER level string for calibration lookup

        Returns:
            (p95, p99) in µs
        """
        key = (fec_mode, ber_level)
        if self._tail_calibrated and key in self._tail_calibration_data:
            cal = self._tail_calibration_data[key]
            alpha_p95 = cal.get("alpha_p95", 1.02)
            alpha_p99 = cal.get("alpha_p99", 1.05)
            return p50 * alpha_p95, p50 * alpha_p99

        # Fallback: conservative heuristic based on error probability
        # Low error probability -> small tail spread
        # Higher error probability -> wider tail (retry/retransmission variance)
        if p_pkt_error > 0:
            # Scale tail spread with P(error) but cap at reasonable bounds
            tail_p95 = 1.0 + min(0.15, p_pkt_error * 1.5)
            tail_p99 = 1.0 + min(0.30, p_pkt_error * 3.0)
        else:
            tail_p95 = 1.02
            tail_p99 = 1.05

        return p50 * tail_p95, p50 * tail_p99

    def calibrate_tails(self, ns3_data: List[Dict]):
        """Fit empirical tail calibration factors from ns-3 P50/P95/P99 data.

        Groups results by (fec_mode, ber_level) and computes
        alpha_p95 = mean(P95/P50) and alpha_p99 = mean(P99/P50).
        """
        groups: Dict[Tuple, List] = {}

        for r in ns3_data:
            p50 = r.get("p50")
            p95 = r.get("p95")
            p99 = r.get("p99")
            if p50 is None or p95 is None or p99 is None or p50 <= 0:
                continue

            ber = r.get("ber", 0)
            fec = r.get("fecEnabled", 0) or (r.get("fecN", 0) > 0)
            retry = r.get("llrEnabled", 0)

            if fec:
                fec_mode = "fec"
            elif retry:
                fec_mode = "retry"
            else:
                fec_mode = "none"

            ber_str = str(ber) if ber > 0 else "0"
            key = (fec_mode, ber_str)
            groups.setdefault(key, []).append((p50, p95, p99))

        for key, ratios in groups.items():
            if len(ratios) < 2:
                continue
            p95_ratios = [r[1] / r[0] for r in ratios]
            p99_ratios = [r[2] / r[0] for r in ratios]
            self._tail_calibration_data[key] = {
                "alpha_p95": sum(p95_ratios) / len(p95_ratios),
                "alpha_p99": sum(p99_ratios) / len(p99_ratios),
                "n_points": len(ratios),
            }

        self._tail_calibrated = len(self._tail_calibration_data) > 0


class AnalyticalSurrogate:
    """Regression-calibrated piecewise latency model with FEC/retry/memory factors."""

    def __init__(self):
        self.calibrated = False
        self.calibration_params = {}

        # Default hardware parameters (H200 NVLink4 native)
        self.aggregate_bw_gbps = 7200.0  # H200 NVLink4 aggregate BW per direction (18 lanes, 900 GB/s peak)
        self.num_gpus = 8
        self.link_latency_ns = 400

        # FEC parameters
        self.fec_n = 0
        self.fec_k = 0
        self.fec_t = 0
        self.fec_encode_latency_ns = 50
        self.fec_decode_latency_ns = 80

        # Retry parameters
        self.llr_enabled = False
        self.llr_mode = "sack"
        self.retry_limit = 3

        # BER
        self.ber = 0.0

        # Memory disaggregation parameters
        self.remote_mem_fraction = 0.0
        self.mem_bandwidth_GBps = 0.0  # 0 = use hardware profile or experiment config
        self.mem_access_latency_ns = 0  # 0 = use hardware profile or experiment config

        # Algorithm/collective
        self.algorithm = "ring"
        self.collective_type = "allreduce"

        # WFQ contention parameters (for queuing theory)
        self.wfq_collective_weight = 0.7
        self.wfq_memory_weight = 0.2
        self.wfq_p2p_weight = 0.1

        # Queuing-theoretic estimator
        self._queuing_estimator = QueuingTheoryEstimator()

        # Calibration coefficients per (algorithm, collective, numGpus)
        # Model: latency_us = alpha + beta * x + gamma * x^2
        # where x = dataSize_bytes / BW_bytes_per_us
        # alpha = startup/latency overhead, beta = BW efficiency, gamma = size-dependent overhead
        self._alpha = None
        self._beta = None
        self._gamma = None

        # Piecewise small-size model: for sizes below threshold, use
        # a separate linear fit with lower startup cost.
        # Small sizes have incomplete pipeline fill, so startup overhead
        # is much lower than the alpha calibrated on large sizes.
        self._alpha_small = None
        self._beta_small = None
        self._small_threshold_bytes = None

    def configure(self, config: Dict):
        """Set model parameters from experiment config dict."""
        optical = config.get("optical", {})
        fec = config.get("fec", {})
        retry = config.get("retry", {})
        hw = config.get("hardware", {})
        topo = config.get("topology", {})
        coll = config.get("collective", {})
        mem = config.get("memory", {})
        contention = config.get("contention", {})

        self.ber = optical.get("ber", 0.0)
        self.fec_n = fec.get("N", 0)
        self.fec_k = fec.get("K", 0)
        self.fec_t = fec.get("T", 0)
        self.fec_encode_latency_ns = fec.get("encodeLatencyNs", 50)
        self.fec_decode_latency_ns = fec.get("decodeLatencyNs", 80)
        self.llr_enabled = retry.get("enabled", False)
        self.llr_mode = retry.get("mode", "sack")
        self.retry_limit = retry.get("retryLimit", 3)
        self.num_gpus = hw.get("numGpus", 4)
        self.algorithm = topo.get("algorithm", "ring")
        self.collective_type = coll.get("type", "allreduce")
        # remoteFraction: check memory dict first, then workload.kvModel fallback
        workload = config.get("workload", {})
        kv_model = workload.get("kvModel", {})
        self.remote_mem_fraction = mem.get("remoteFraction",
                                            kv_model.get("remoteFraction", 0.0))
        self.mem_bandwidth_GBps = mem.get("bandwidthGBps", 0.0)
        # accessLatencyNs: also check memory.latencyNs fallback
        self.mem_access_latency_ns = mem.get("accessLatencyNs",
                                              mem.get("latencyNs", 0))
        self.wfq_collective_weight = contention.get("collectiveWeight", self.wfq_collective_weight)
        self.wfq_memory_weight = contention.get("memoryWeight", self.wfq_memory_weight)
        self.wfq_p2p_weight = contention.get("p2pWeight", self.wfq_p2p_weight)

        # Load hardware profile (shipped under surrogate/analytical/profiles/)
        hw_profile = hw.get("profile", "h200")
        hw_path = Path(__file__).parent / "profiles" / f"{hw_profile}.json"
        if hw_path.exists():
            with open(hw_path) as f:
                hw_data = json.load(f)
            nvlink = hw_data.get("nvlink", {})
            agg_bw = nvlink.get("aggregateBandwidthGBps", 0)
            per_link_gb = nvlink.get("perLinkEffectiveBandwidthGBps", 0)
            lanes = nvlink.get("lanesPerGpu", nvlink.get("numLinks", 1))
            if agg_bw > 0:
                self.aggregate_bw_gbps = agg_bw * 8  # GB/s to Gbps per direction
            elif per_link_gb > 0 and lanes > 0:
                self.aggregate_bw_gbps = per_link_gb * 8 * lanes  # per-link × lanes
            self.link_latency_ns = nvlink.get("latencyNs", 500)
            # Load memory semantic defaults from hardware profile
            mem_sem = hw_data.get("memorySemantic", {})
            if self.mem_access_latency_ns == 0:
                self.mem_access_latency_ns = mem_sem.get("syncMemLatencyNs", 500)
            if self.mem_bandwidth_GBps == 0:
                # Default memory BW: aggregate NVLink BW (remote reads via NVLink)
                self.mem_bandwidth_GBps = self.aggregate_bw_gbps

        # Use calibration coefficients if available
        key = (self.algorithm, self.collective_type, self.num_gpus)
        if key in self.calibration_params:
            self._alpha = self.calibration_params[key]["alpha"]
            self._beta = self.calibration_params[key]["beta"]
            self._gamma = self.calibration_params[key].get("gamma", 0.0)
            self._alpha_small = self.calibration_params[key].get("alpha_small")
            self._beta_small = self.calibration_params[key].get("beta_small")
            self._small_threshold_bytes = self.calibration_params[key].get("small_threshold_bytes")
            # Preserve calibrated BW (don't override from hardware profile)
            self._calibrated_bw = self.calibration_params[key].get("bw_gbps", self.aggregate_bw_gbps)
            self.aggregate_bw_gbps = self._calibrated_bw
        else:
            self._alpha = None
            self._beta = None
            self._gamma = None
            self._alpha_small = None
            self._beta_small = None
            self._small_threshold_bytes = None

    def packet_error_probability(self, packet_size_bytes: int) -> float:
        """Probability that a packet has at least one bit error from BER."""
        if self.ber <= 0:
            return 0.0
        bits = packet_size_bytes * 8
        if self.ber * bits < 0.01:
            return self.ber * bits
        return 1.0 - math.pow(1.0 - self.ber, bits)

    def post_fec_uncorrectable(self, pre_fec_ber: float) -> float:
        """Post-FEC uncorrectable codeword probability using binomial model."""
        if self.fec_n == 0 or self.fec_t == 0:
            return self.packet_error_probability(4096) if pre_fec_ber > 0 else 0.0

        symbol_bits = 10  # RS symbol size
        p_sym = 1.0 - math.pow(1.0 - pre_fec_ber, symbol_bits)
        if p_sym < 1e-15:
            return 0.0

        prob = 0.0
        for i in range(self.fec_t + 1, self.fec_n + 1):
            # log(C(n,k)) = lgamma(n+1) - lgamma(k+1) - lgamma(n-k+1)
            log_c = math.lgamma(self.fec_n + 1) - math.lgamma(i + 1) - math.lgamma(self.fec_n - i + 1)
            log_t = log_c + i * math.log(p_sym) + (self.fec_n - i) * math.log(1 - p_sym)
            prob += math.exp(log_t)
        return prob

    def fec_bw_overhead(self) -> float:
        """FEC bandwidth expansion factor: N/K."""
        if self.fec_k == 0:
            return 1.0
        return self.fec_n / self.fec_k

    def effective_bw_gbps(self) -> float:
        """Aggregate BW after FEC overhead."""
        return self.aggregate_bw_gbps / self.fec_bw_overhead()

    def retry_amplification(self, data_size_bytes: int) -> float:
        """GBN cascade amplification factor for retry-only configs.

        In ring topology, ns-3 sends each segment (dataSize/numGpus) as ONE
        unfragmented packet, so BER checks happen at segment granularity, not
        4KB packet granularity. P(segment_error) = 1 - exp(-BER * segBits).

        GBN cascade: each segment error triggers a full-step retry (retransmit
        from NACK point), which blocks the pipeline for remaining steps. The
        cascade factor scales with segment_transfer_time because larger segments
        cause longer pipeline stalls per error event.

        Model: amp = 1 + P(seg_error) * seg_transfer_time * GBN_CASCADE_C / baseline
        where GBN_CASCADE_C = 155 (calibrated from ns-3 optical validation data).

        For FEC-protected configs, the amplification comes from post-FEC
        uncorrectable codeword errors (much smaller P, negligible cascade).
        """
        if self.ber <= 0:
            return 1.0

        # FEC-protected configs: post-FEC P(error) is very small, cascade negligible
        if self.fec_n > 0 and self.fec_t > 0:
            p_unc = self.post_fec_uncorrectable(self.ber)
            n_codewords = max(1, data_size_bytes / self.fec_k) if self.fec_k > 0 else 1
            p_pkt_error = 1.0 - math.pow(1.0 - p_unc, n_codewords)
            if p_pkt_error <= 0:
                return 1.0
            if p_pkt_error >= 1.0:
                return float('inf')
            # FEC: small P, geometric model sufficient (no cascade)
            if self.retry_limit <= 0:
                return 1.0 / (1.0 - p_pkt_error)
            return (1.0 - math.pow(p_pkt_error, self.retry_limit + 1)) / (1.0 - p_pkt_error)

        # Unprotected configs (retry-only / no-FEC): segment-level P(error) + GBN cascade
        if not self.llr_enabled:
            # No retry mechanism: any segment error stalls the collective
            seg_size = data_size_bytes / self.num_gpus
            p_seg = self.packet_error_probability(int(seg_size))
            if p_seg <= 0:
                return 1.0
            if p_seg >= 1.0:
                return float('inf')
            return 1.0 + p_seg  # One wasted transmission per error probability

        # Retry-only with GBN: segment-level P(error) + cascade amplification
        seg_size = data_size_bytes / self.num_gpus
        p_seg = self.packet_error_probability(int(seg_size))

        if p_seg <= 0:
            return 1.0
        if p_seg >= 1.0:
            return float('inf')

        # GBN cascade model: amp = 1 + P(seg_error) * cascade_factor
        # cascade_factor = seg_transfer_time * GBN_CASCADE_C / baseline
        # where baseline = calibrated base_latency (startup + transfer)
        GBN_CASCADE_C = 155.0

        bw_bytes_per_us = self.aggregate_bw_gbps / 8 * 1000
        seg_transfer_us = seg_size / bw_bytes_per_us
        baseline_us = self.base_latency_us(data_size_bytes)

        cascade_factor = seg_transfer_us * GBN_CASCADE_C / baseline_us

        amp = 1.0 + p_seg * cascade_factor

        # Cap amplification for extreme cases (P near 1.0, retry exhaustion)
        if self.retry_limit > 0:
            max_amp = 1.0 + (1.0 - math.pow(p_seg, self.retry_limit + 1)) / max(0.001, 1.0 - p_seg) * 2.0
            amp = min(amp, max_amp)

        return amp

    def fec_latency_us(self, data_size_bytes: int) -> float:
        """FEC encode+decode latency overhead in µs.

        In ns-3, FEC encode/decode latency is per-packet (not per-codeword).
        Packets are pipelined, so total latency ≈ num_steps * (encode + decode)
        per packet, with each step adding one encode+decode pipeline slot.
        """
        if self.fec_n == 0 or self.fec_k == 0:
            return 0.0
        # Per-packet encode+decode latency (pipeline depth)
        pkt_latency_ns = self.fec_encode_latency_ns + self.fec_decode_latency_ns
        # Number of steps in the collective determines pipeline overhead
        if self.algorithm == "ring" and self.collective_type == "allreduce":
            num_steps = 2 * (self.num_gpus - 1)
        elif self.algorithm == "ring":
            num_steps = self.num_gpus - 1
        else:
            num_steps = 1
        # Total FEC overhead = pipeline depth * per-packet latency
        # (each step adds one pipeline slot of FEC encode+decode)
        return num_steps * pkt_latency_ns / 1000.0

    def memory_overhead_us(self, data_size_bytes: int) -> float:
        """Memory disaggregation latency overhead in µs.

        Uses hardware-profile-derived memory BW and latency as defaults,
        overridden by experiment config if specified.
        """
        if self.remote_mem_fraction <= 0:
            return 0.0
        if self.mem_bandwidth_GBps <= 0:
            return 0.0  # No memory bandwidth configured
        remote_bytes = data_size_bytes * self.remote_mem_fraction
        mem_bw_bytes_per_us = self.mem_bandwidth_GBps * 1000  # GB/s → bytes/µs
        transfer_us = remote_bytes / mem_bw_bytes_per_us
        access_us = self.mem_access_latency_ns / 1000.0
        return transfer_us + access_us

    def base_latency_us(self, data_size_bytes: int) -> float:
        """Base latency (BER=0, no FEC/retry, no remote memory) using calibration.

        Uses aggregate_bw (without FEC overhead) because the beta coefficient
        was calibrated on aggregate BW. FEC BW overhead is applied separately
        in predict_latency_us.

        Piecewise model: for sizes below the small-size threshold, uses a
        separate (alpha_small, beta_small) fit that captures incomplete
        pipeline fill. For sizes above threshold, uses the standard model.
        """
        bw_bytes_per_us = self.aggregate_bw_gbps / 8 * 1000

        if self._alpha is not None and self._beta is not None:
            x = data_size_bytes / bw_bytes_per_us

            # Piecewise: use small-size model below threshold
            if self._alpha_small is not None and self._small_threshold_bytes is not None \
                    and data_size_bytes < self._small_threshold_bytes:
                return self._alpha_small + self._beta_small * x

            # Standard model for BW-dominated sizes
            return self._alpha + self._beta * x + self._gamma * x * x

        # Default analytical model (uncalibrated)
        startup_us = self.link_latency_ns / 1000.0 * 2 * (self.num_gpus - 1) + 5.0
        link_latency_us = self.link_latency_ns / 1000.0

        if self.algorithm == "ring" and self.collective_type == "allreduce":
            steps = 2 * (self.num_gpus - 1)
            per_step_size = data_size_bytes * (self.num_gpus - 1) / self.num_gpus / steps
            transfer_us = steps * per_step_size / bw_bytes_per_us
        elif self.algorithm == "ring":
            steps = self.num_gpus - 1
            transfer_us = steps * data_size_bytes / bw_bytes_per_us / self.num_gpus
        else:
            transfer_us = data_size_bytes / bw_bytes_per_us

        return startup_us + link_latency_us * max(1, steps) + transfer_us

    def predict_latency_us(self, data_size_bytes: int) -> Dict:
        """Predict P50/P95/P99 latency with component breakdown."""
        base = self.base_latency_us(data_size_bytes)
        retry_amp = self.retry_amplification(data_size_bytes)
        fec_lat = self.fec_latency_us(data_size_bytes)
        mem_lat = self.memory_overhead_us(data_size_bytes)
        fec_bw = self.fec_bw_overhead()  # N/K multiplier (1.058 for RS(544,514,15))

        # Determine which model region we're in
        use_small_model = (self._alpha_small is not None
                           and self._small_threshold_bytes is not None
                           and data_size_bytes < self._small_threshold_bytes)
        alpha_used = self._alpha_small if use_small_model else self._alpha
        beta_used = self._beta_small if use_small_model else self._beta
        gamma_used = 0.0 if use_small_model else self._gamma

        # Compute raw transfer time (single calculation, reused for p50 and breakdown)
        calibrated = self._alpha is not None and self._beta is not None
        bw_bytes_per_us = self.aggregate_bw_gbps / 8 * 1000
        if calibrated:
            x = data_size_bytes / bw_bytes_per_us
            raw_transfer = beta_used * x + gamma_used * x * x
            fec_bw_overhead_us = raw_transfer * (fec_bw - 1.0)
            total_transfer = (raw_transfer + fec_bw_overhead_us) * retry_amp
            p50 = alpha_used + total_transfer + fec_lat + mem_lat
        else:
            raw_transfer = base - (alpha_used if alpha_used is not None else 65.0)
            p50 = base * retry_amp + fec_lat + mem_lat

        # Handle infinite latency (collective fails)
        if retry_amp >= 100 or p50 >= 1e6:
            return {
                "p50": float('inf'), "p95": float('inf'), "p99": float('inf'),
                "breakdown": {"base_us": base, "retry_factor": retry_amp,
                              "fec_latency_us": fec_lat, "memory_us": mem_lat},
                "data_size_bytes": data_size_bytes, "ber": self.ber,
                "status": "failed",
            }

        # Queuing-theoretic tail estimation: P95/P99 via calibrated tail scaling
        # Uses M/G/1 PK for mean wait + empirical calibration for tails.
        # Does NOT claim analytical P95/P99 from PK/Kingman (they give mean only).
        p_pkt_error = 0.0
        if self.ber > 0:
            if self.fec_n > 0 and self.fec_t > 0:
                p_pkt_error = self.post_fec_uncorrectable(self.ber)
                fec_mode = "fec"
            else:
                seg_size = data_size_bytes / self.num_gpus
                p_pkt_error = self.packet_error_probability(int(seg_size))
                fec_mode = "retry" if self.llr_enabled else "none"
        else:
            fec_mode = "none"

        # Add queuing delay contributions when contention is present
        # Note: additive composition is an approximation; for NVLink fabrics
        # where collective and memory traffic share links, the independent
        # additive waits may slightly overestimate contention. This is
        # acknowledged as a modeling simplification.
        queuing_delay_us = 0.0
        if self.remote_mem_fraction > 0:
            # Memory queueing: M/D/1 for deterministic service
            queuing_delay_us += self._queuing_estimator.memory_queueing_delay(
                data_size_bytes, self.remote_mem_fraction,
                self.mem_bandwidth_GBps, self.mem_access_latency_ns)
            # Collective queueing: WFQ reduces effective BW when memory is active
            # Delay is proportional to transfer component, not full base latency
            collective_weight = self.wfq_collective_weight
            if collective_weight > 0 and collective_weight < 1.0:
                wfq_delay = raw_transfer * (1.0 / collective_weight - 1.0) * self.remote_mem_fraction
                queuing_delay_us += wfq_delay

        # Add queuing delay to P50 (not just to tail estimation)
        p50 += queuing_delay_us

        # P95/P99: empirical tail calibration (not PK analytical tails)
        ber_str = str(self.ber) if self.ber > 0 else "0"
        p95, p99 = self._queuing_estimator.estimate_tail_percentiles(
            p50, p_pkt_error, fec_mode, ber_str)

        return {
            "p50": p50, "p95": p95, "p99": p99,
            "breakdown": {
                "base_us": base,
                "startup_us": alpha_used if alpha_used is not None else 65.0,
                "transfer_us": raw_transfer,
                "amplified_transfer_us": raw_transfer * (retry_amp - 1),
                "fec_bw_overhead_us": raw_transfer * (fec_bw - 1.0) if calibrated else 0.0,
                "fec_latency_us": fec_lat,
                "memory_us": mem_lat,
                "retry_factor": retry_amp,
                "fec_bw_factor": fec_bw,
            },
            "data_size_bytes": data_size_bytes,
            "ber": self.ber,
            "fec": f"({self.fec_n},{self.fec_k},{self.fec_t})" if self.fec_n > 0 else "none",
            "retry": f"{'off' if not self.llr_enabled else self.llr_mode}",
            "status": "success",
        }

    def calibrate(self, ns3_data: List[Dict]):
        """Fit alpha and beta regression coefficients from ns-3 baseline data.

        Uses linear regression on: latency = alpha + beta * size/BW
        Only uses BER=0, no-FEC, no-retry results (baseline config).

        Also fits a piecewise small-size model (alpha_small, beta_small) for
        data points below a BW-dominated threshold, capturing the incomplete
        pipeline fill that causes lower startup cost at small sizes.
        """
        # Collect baseline data: (size, latency_us) pairs
        baseline = []
        for r in ns3_data:
            ber = r.get("ber", 0)
            fec = r.get("fecEnabled", 0)
            retry = r.get("llrEnabled", 0)
            t = r.get("simTimeUs", 0)
            sz = r.get("dataSize", 0)
            algo = r.get("algorithm", "ring")
            coll = r.get("collective", "allreduce") if "collective" in r else "allreduce"
            ngpus = r.get("numGpus", 4)

            if t is None or t <= 0 or sz <= 0:
                continue
            # Only use pure baseline (BER=0, no FEC, no retry)
            if ber > 0 or fec > 0 or retry > 0:
                continue
            baseline.append((sz, t, algo, coll, ngpus))

        if len(baseline) < 3:
            print(f"Warning: only {len(baseline)} baseline points, calibration may be unreliable")

        # Group by (algorithm, collective, numGpus)
        groups: Dict[Tuple, List] = {}
        for sz, t, algo, coll, ngpus in baseline:
            key = (algo, coll, ngpus)
            groups.setdefault(key, []).append((sz, t))

        # Fit regression for each group: latency = alpha + beta * x + gamma * x^2
        bw_bytes_per_us = self.aggregate_bw_gbps / 8 * 1000
        for key, points in groups.items():
            sizes = np.array([p[0] for p in points])
            latencies = np.array([p[1] for p in points])
            # X = size / BW_bytes_per_us (normalized transfer time)
            x = sizes / bw_bytes_per_us
            x2 = x * x
            y = latencies

            # Determine BW-dominated threshold: size where startup < 50% of latency
            # Use 1MB as default threshold (startup-dominated region boundary)
            threshold_bytes = 4194304  # 4MB — BW-dominated region start

            # Split into small and large regions
            small_mask = sizes < threshold_bytes
            large_mask = sizes >= threshold_bytes

            # Fit BW-dominated model on large sizes
            large_sizes = sizes[large_mask]
            large_x = x[large_mask]
            large_x2 = large_x * large_x
            large_y = y[large_mask]

            if len(large_sizes) >= 3:
                # Quadratic regression: y = alpha + beta * x + gamma * x^2
                A = np.column_stack([np.ones(len(large_x)), large_x, large_x2])
                result = np.linalg.lstsq(A, large_y, rcond=None)
                coeffs = result[0]
                alpha = coeffs[0]  # Startup + latency overhead (µs)
                beta = coeffs[1]   # BW efficiency factor
                gamma = coeffs[2]  # Size-dependent overhead factor
            elif len(large_sizes) >= 2:
                # Linear regression (insufficient points for quadratic)
                A = np.column_stack([np.ones(len(large_x)), large_x])
                result = np.linalg.lstsq(A, large_y, rcond=None)
                coeffs = result[0]
                alpha = coeffs[0]
                beta = coeffs[1]
                gamma = 0.0
            else:
                # Use all data points for standard fit
                A = np.column_stack([np.ones(len(x)), x, x2])
                result = np.linalg.lstsq(A, y, rcond=None)
                coeffs = result[0]
                alpha = coeffs[0]
                beta = coeffs[1]
                gamma = coeffs[2]
                threshold_bytes = None  # No piecewise model

            # Fit small-size model on small sizes (linear: simpler for few points)
            small_sizes = sizes[small_mask]
            small_x = x[small_mask]
            small_y = y[small_mask]

            alpha_small = None
            beta_small = None
            if len(small_sizes) >= 2 and threshold_bytes is not None:
                A_small = np.column_stack([np.ones(len(small_x)), small_x])
                result_small = np.linalg.lstsq(A_small, small_y, rcond=None)
                alpha_small = float(result_small[0][0])
                beta_small = float(result_small[0][1])
                print(f"  Small-size model ({key[0]}, {key[1]}, {key[2]}G): "
                      f"alpha_small={alpha_small:.2f}µs, beta_small={beta_small:.3f}, "
                      f"n={len(small_sizes)} points")

            self.calibration_params[key] = {
                "alpha": float(alpha),
                "beta": float(beta),
                "gamma": float(gamma),
                "num_points": len(points),
                "bw_gbps": self.aggregate_bw_gbps,
                "alpha_small": alpha_small,
                "beta_small": beta_small,
                "small_threshold_bytes": threshold_bytes,
            }
            print(f"Calibrated ({key[0]}, {key[1]}, {key[2]}G): "
                  f"alpha={alpha:.2f}µs, beta={beta:.3f}, gamma={gamma:.6f}, "
                  f"n={len(points)} points")

        self.calibrated = True
        # Calibrate tail factors from ns-3 P50/P95/P99 data (if available)
        self._queuing_estimator.calibrate_tails(ns3_data)
        # Set current coefficients (all active fields)
        cur_key = (self.algorithm, self.collective_type, self.num_gpus)
        if cur_key in self.calibration_params:
            params = self.calibration_params[cur_key]
            self._alpha = params["alpha"]
            self._beta = params["beta"]
            self._gamma = params.get("gamma", 0.0)
            self._alpha_small = params.get("alpha_small")
            self._beta_small = params.get("beta_small")
            self._small_threshold_bytes = params.get("small_threshold_bytes")

    def calibrate_from_file(self, filepath: str):
        """Load calibration data from a JSON file and fit coefficients."""
        with open(filepath) as f:
            data = json.load(f)
        self.calibrate(data)

    def validate(self, ns3_data: List[Dict], tolerance_pct: float = 10.0) -> Dict:
        """Validate surrogate predictions against ns-3 results.

        Returns percentage of predictions within tolerance of ns-3,
        with per-category (FEC/retry-only/no-FEC) breakdown.
        """
        within = 0
        total = 0
        errors = []
        details = []
        category_errors = defaultdict(list)

        for r in ns3_data:
            t = r.get("simTimeUs", None)
            if t is None or t <= 0:
                continue

            sz = r.get("dataSize", 1048576)
            algo = r.get("algorithm", "ring")
            coll = r.get("collective", "allreduce") if "collective" in r else "allreduce"
            ngpus = r.get("numGpus", 4)
            ber = r.get("ber", 0)
            fec = r.get("fecEnabled", 0) or (r.get("fecN", 0) > 0)
            retry = r.get("llrEnabled", 0)
            remote_frac = r.get("remoteMemFraction", r.get("remoteFrac", 0))

            # Classify into resilience category
            if fec:
                category = "fec_protected"
            elif retry and not fec:
                category = "retry_only"
            else:
                category = "no_fec"

            # Configure surrogate for this run
            config = {
                "hardware": {"numGpus": ngpus, "profile": "h200"},
                "topology": {"algorithm": algo},
                "collective": {"type": coll},
                "optical": {"ber": ber},
                "fec": {"N": 544 if fec else 0, "K": 514 if fec else 0, "T": 15 if fec else 0},
                "retry": {"enabled": bool(retry), "mode": "sack"},
                "memory": {"remoteFraction": remote_frac},
            }
            self.configure(config)
            pred = self.predict_latency_us(sz)

            if pred["p50"] == float('inf') or pred["p50"] <= 0:
                continue

            err_pct = abs(pred["p50"] - t) / t * 100
            errors.append(err_pct)
            category_errors[category].append(err_pct)
            details.append({
                "size": sz, "ber": ber, "fec": fec, "retry": retry,
                "remote_frac": remote_frac, "category": category,
                "ns3_us": t, "pred_p50_us": pred["p50"],
                "pred_p95_us": pred.get("p95", 0),
                "pred_p99_us": pred.get("p99", 0),
                "error_pct": err_pct,
            })
            if err_pct <= tolerance_pct:
                within += 1
            total += 1

        if total == 0:
            return {"within_pct": 0, "avg_error_pct": 0, "max_error_pct": 0, "total": 0,
                    "category_breakdown": {}, "details": details}

        # Compute per-category statistics
        category_breakdown = {}
        for cat, cat_errs in category_errors.items():
            cat_avg = sum(cat_errs) / len(cat_errs)
            cat_max = max(cat_errs)
            cat_within = sum(1 for e in cat_errs if e <= tolerance_pct)
            category_breakdown[cat] = {
                "count": len(cat_errs),
                "avg_error_pct": cat_avg,
                "max_error_pct": cat_max,
                "within_pct": cat_within / len(cat_errs) * 100,
                "within_count": cat_within,
            }

        return {
            "within_pct": within / total * 100,
            "avg_error_pct": sum(errors) / len(errors),
            "max_error_pct": max(errors),
            "median_error_pct": sorted(errors)[len(errors)//2],
            "total": total,
            "within_count": within,
            "category_breakdown": category_breakdown,
            "details": details,
        }

    def save_calibration(self, filepath: str):
        """Save calibration parameters to JSON, including tail calibration."""
        out = {
            "calibrated": self.calibrated,
            "params": {},
            "tail_calibration": {
                "calibrated": self._queuing_estimator._tail_calibrated,
                "groups": {},
            },
        }
        for key, val in self.calibration_params.items():
            str_key = f"{key[0]}_{key[1]}_{key[2]}gpu"
            clean_val = {k: v for k, v in val.items() if v is not None}
            out["params"][str_key] = clean_val
        for gkey, gval in self._queuing_estimator._tail_calibration_data.items():
            str_gkey = f"{gkey[0]}_{gkey[1]}"
            out["tail_calibration"]["groups"][str_gkey] = gval
        with open(filepath, 'w') as f:
            json.dump(out, f, indent=2)

    def load_calibration(self, filepath: str):
        """Load pre-calibrated parameters from JSON, including tail calibration."""
        with open(filepath) as f:
            data = json.load(f)
        self.calibrated = data.get("calibrated", False)
        for str_key, val in data.get("params", {}).items():
            parts = str_key.split("_")
            algo = parts[0]
            coll = parts[1]
            ngpus = int(parts[2].replace("gpu", ""))
            key = (algo, coll, ngpus)
            self.calibration_params[key] = val
        cur_key = (self.algorithm, self.collective_type, self.num_gpus)
        if cur_key in self.calibration_params:
            params = self.calibration_params[cur_key]
            self._alpha = params["alpha"]
            self._beta = params["beta"]
            self._gamma = params.get("gamma", 0.0)
            self._alpha_small = params.get("alpha_small")
            self._beta_small = params.get("beta_small")
            self._small_threshold_bytes = params.get("small_threshold_bytes")
        tail_data = data.get("tail_calibration", {})
        self._queuing_estimator._tail_calibrated = tail_data.get("calibrated", False)
        for str_gkey, gval in tail_data.get("groups", {}).items():
            parts = str_gkey.split("_")
            gkey = (parts[0], parts[1])
            self._queuing_estimator._tail_calibration_data[gkey] = gval


def main():
    """Calibrate and validate the analytical surrogate against the H200 ring
    baseline, then run an optical-reliability demo under a selectable hardware
    profile."""
    import argparse
    profiles_dir = Path(__file__).parent / "profiles"
    profile_choices = sorted(p.stem for p in profiles_dir.glob("*.json"))
    parser = argparse.ArgumentParser(
        description="Calibrate the analytical surrogate on the shipped H200 "
                    "baseline and run an optical-reliability demo under a "
                    "hardware profile.")
    parser.add_argument("--profile", default="h200", choices=profile_choices,
                        help="hardware profile for the optical-reliability demo "
                             "(choices: %(choices)s). Calibration still fits on the "
                             "shipped H200 baseline, which is the only one in-repo.")
    args = parser.parse_args()

    surrogate = AnalyticalSurrogate()
    cal_dir = Path(__file__).parent.parent / "calibration"
    cal_path = cal_dir / "h200_ring4_ar_baseline.json"
    if not cal_path.exists():
        print(f"H200 ring4 baseline not found at {cal_path}")
        return
    with open(cal_path) as f:
        cal_data = json.load(f)
    for r in cal_data:
        r["ber"] = 0
        r["fecEnabled"] = 0
        r["llrEnabled"] = 0
        r["algorithm"] = "ring"
        r["numGpus"] = 4
        r["collective"] = "allreduce"
    surrogate.calibrate(cal_data)
    val = surrogate.validate(cal_data)
    print("Validation against H200 ring4 baseline:")
    print(f"  Within 10%: {val['within_pct']:.1f}% ({val['within_count']}/{val['total']})")
    print(f"  Avg error: {val['avg_error_pct']:.1f}%")
    print(f"  Max error: {val['max_error_pct']:.1f}%")
    print(f"\n{'Size':>12} {'ns3(us)':>10} {'pred(us)':>10} {'error%':>8}")
    print("-" * 45)
    for d in sorted(val["details"], key=lambda x: x["size"]):
        print(f"{d['size']:>12} {d['ns3_us']:>10.1f} {d['pred_p50_us']:>10.1f} {d['error_pct']:>8.1f}")
    out_path = cal_dir / "h200_ring4_surrogate_calibration_regenerated.json"
    surrogate.save_calibration(str(out_path))
    print(f"\nCalibration saved to {out_path}")

    if args.profile != "h200":
        print(f"\nNote: --profile={args.profile} loads that profile's memory-semantic "
              f"defaults for the demo below. The ring-allreduce prediction is "
              f"BW-bound; its BW and startup come from the shipped H200 calibration "
              f"and are NOT overridden, so this demo is ~unchanged for non-H200 "
              f"profiles. Vendor interconnect params (MetaXLink/UB/HCCS/...) are "
              f"not yet consumed by this surrogate path. No {args.profile} "
              f"calibration data ships in this repo.")

    # Optical reliability demo: FEC/retry amplification on a 1 MB allreduce.
    print(f"\n--- Optical Reliability Predictions ({args.profile}, 1 MB allreduce, 4 GPU) ---")
    test_configs = [
        ("baseline (BER=0)", 0, False, False),
        ("FEC+SACK (BER=1e-9)", 1e-9, True, True),
        ("FEC+SACK (BER=1e-8)", 1e-8, True, True),
        ("retry-only (BER=1e-8)", 1e-8, False, True),
    ]
    for name, ber, fec, retry in test_configs:
        surrogate.configure({
            "hardware": {"numGpus": 4, "profile": args.profile},
            "topology": {"algorithm": "ring"},
            "collective": {"type": "allreduce"},
            "optical": {"ber": ber},
            "fec": {"N": 544 if fec else 0, "K": 514 if fec else 0, "T": 15 if fec else 0},
            "retry": {"enabled": retry, "mode": "sack"},
            "memory": {"remoteFraction": 0},
        })
        result = surrogate.predict_latency_us(1048576)
        p50 = result["p50"] if result["p50"] != float('inf') else "FAIL"
        print(f"  {name:<25} P50={p50:>8} us  retry_factor={result['breakdown']['retry_factor']:.2f}")


if __name__ == "__main__":
    main()