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
   T_tree = D(N-1)/(N B_tree log2(N))
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


class TheoryDerivedSurrogate:
    """Factorized analytical surrogate with a fixed platform profile.

    Configured or platform-profiled parameters:
      - packet_bw: physical packet bandwidth used by retransmission
      - ring_bw: effective ring transfer bandwidth (bytes/µs)
      - tree_bw_per_level: effective tree bandwidth per active level
      - nvls_bw: aggregate NVLS transfer-work bandwidth
      - lat: link propagation latency (µs)
      - credit_bdp_ring/tree: packets needed to fill the schedule path
      - chunk_size: spray chunk size (bytes)
      - num_lanes: NVLink lanes per GPU
      - T_min_switch: NVSwitch pipeline fill (VOQ + arbiter, µs)
      - T_fec_encode, T_fec_decode: FEC pipeline latency (µs)
    """

    def __init__(self,
                 packet_bw_bytes_per_us=177000,
                 ring_bw_bytes_per_us=94000,
                 tree_bw_per_level_bytes_per_us=26250,
                 nvls_bw_bytes_per_us=535500,
                 link_latency_us=0.4,
                 credit_bdp_ring_packets=91.5,
                 credit_bdp_tree_packets=35.5,
                 spray_chunk_bytes=131072,
                 bulk_chunk_bytes=8 * 1024 * 1024,
                 num_lanes=18,
                 t_min_switch_us=1.1,
                 fec_n=544, fec_k=514, fec_t=15,
                 fec_symbol_bits=10,
                 fec_encode_ns=50, fec_decode_ns=80,
                 startup_us=15.0,
                 nvls_startup_us=23.0,
                 retry_source_bw_bytes_per_us=4_800_000,
                 retry_source_latency_us=0.3):
        # Fixed profile parameters from hardware, ns-3 configuration, or
        # one-time platform calibration.
        self.BW_link = packet_bw_bytes_per_us
        self.ring_bw = ring_bw_bytes_per_us
        self.tree_bw_per_level = tree_bw_per_level_bytes_per_us
        self.nvls_bw = nvls_bw_bytes_per_us
        self.lat = link_latency_us
        self.credit_bdp_ring = credit_bdp_ring_packets
        self.credit_bdp_tree = credit_bdp_tree_packets
        self.chunk_size = spray_chunk_bytes
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

    # ---- Topology helpers ----

    def _steps(self, N, algo):
        if algo == 'ring':
            return 2 * (N - 1)
        elif algo == 'tree':
            return 2 * max(1, int(math.log2(N)))
        elif algo == 'nvls':
            return 2
        else:
            return 2 * (N - 1)

    def _per_step(self, D, N, algo, steps):
        if algo == 'nvls':
            return D / N
        return D * (N - 1) / N / steps

    def _bdp(self, N, algo):
        """Packets required to keep the selected schedule path busy."""
        if algo == 'ring':
            return self.credit_bdp_ring
        if algo == 'tree':
            return self.credit_bdp_tree
        return float('inf')

    # ---- Step time (T_step) ----

    def _step_time(self, per_step, N, algo):
        """Per-step transfer time (µs).

        Ring: T_step = per_step / B_ring + lat
        Tree: T_step = T_min_switch + per_step / (B_tree × log₂N)
        NVLS: T_step = (N × per_step) / B_nvls + lat
        """
        if algo == 'tree':
            log2N = max(1, int(math.log2(N)))
            bw_tree = self.tree_bw_per_level * log2N
            return self.T_min_switch + per_step / bw_tree
        elif algo == 'nvls':
            return (per_step * N) / self.nvls_bw + self.lat
        else:
            return per_step / self.ring_bw + self.lat

    # ---- Credit pressure (Phi_credit) ----

    def _credit_pressure(self, D, N, algo, cr):
        """Sliding-window flow control pressure.

        Credit-controlled paths: Phi = max(1, BDP / cr)
        NVLS: Phi = 1 (the modeled NVLS path bypasses endpoint credits)
        """
        if algo == 'nvls' or cr <= 0:
            return 1.0

        bdp = self._bdp(N, algo)
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

    def _wire_bytes_per_transfer(self, data_bytes, num_gpus, algo):
        """Wire bytes in one protocol transfer before FEC encoding."""
        transfer_bytes = data_bytes / max(1, num_gpus)
        if algo == 'nvls':
            return transfer_bytes
        if transfer_bytes < 8192:
            return math.ceil(transfer_bytes / 64) * 128
        if transfer_bytes < 2 * 1024 * 1024:
            return math.ceil(transfer_bytes / 120) * 128
        return transfer_bytes

    def _packets_per_transfer(self, data_bytes, num_gpus, algo):
        wire_bytes = self._wire_bytes_per_transfer(data_bytes, num_gpus, algo)
        size_limited = math.ceil(wire_bytes / self.bulk_chunk_size)
        return max(1, self.num_lanes, size_limited)

    def _packet_bytes_per_transfer(self, data_bytes, num_gpus, algo):
        wire_bytes = self._wire_bytes_per_transfer(data_bytes, num_gpus, algo)
        return wire_bytes / self._packets_per_transfer(
            data_bytes, num_gpus, algo)

    def required_retry_buffer_entries(self, data_bytes, num_gpus, algo):
        """Maximum packets an endpoint may need to retain for one tree step."""
        packets = self._packets_per_transfer(data_bytes, num_gpus, algo)
        return 2 * packets if algo == 'tree' else packets

    def required_retry_buffer_bytes(self, data_bytes, num_gpus, algo):
        """Bytes needed to retain the modeled transfer fragments."""
        return (
            self.required_retry_buffer_entries(data_bytes, num_gpus, algo)
            * self._packet_bytes_per_transfer(data_bytes, num_gpus, algo)
        )

    def _retry_buffer_capacity_entries(self, retry_buffer_entries=None,
                                       retry_buffer_bytes=None):
        if retry_buffer_bytes is None:
            return retry_buffer_entries
        byte_limited_entries = max(0, int(retry_buffer_bytes // self.chunk_size))
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
                                  optical_hops=1.0):
        """Expected packets that exhaust the configured retry limit."""
        if retry_limit is None or retry_limit < 0:
            return 0.0
        packet_bytes = self._packet_bytes_per_transfer(
            data_bytes, num_gpus, algo)
        p_link = self._packet_failure_probability(
            ber, fec_enabled, fec_n, fec_k, fec_t, packet_bytes)
        if p_link <= 0:
            return 0.0
        p_path = 1.0 - (1.0 - p_link) ** max(1.0, optical_hops)
        packets_per_transfer = self._packets_per_transfer(
            data_bytes, num_gpus, algo)
        if algo in ('tree', 'ring'):
            transfer_count = 2 * (num_gpus - 1)
        else:
            transfer_count = num_gpus
        attempts = retry_limit + 1
        return (transfer_count * packets_per_transfer
                * min(1.0, max(0.0, optical_fraction))
                * p_path ** attempts)

    # ---- Shared-resource contention (Phi_share) ----

    def _shared_contention(self, algo, cr, competing_bw, D, N):
        """Residual-capacity penalty at a shared bottleneck.

        ``competing_bw`` is the traffic rate, in GB/s, that uses the same
        bottleneck as the modeled transfer. The expression has no fitted
        contention coefficient.
        """
        if competing_bw <= 0:
            return 1.0

        if algo == 'tree':
            schedule_bw = self.tree_bw_per_level * max(1, int(math.log2(N)))
        elif algo == 'nvls':
            schedule_bw = self.nvls_bw
        else:
            schedule_bw = self.ring_bw
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
                        optical_fraction=1.0, optical_hops=1.0):
        """Delay from packet attempts that extend a protocol step.

        A step completes after all of its parallel packet fragments arrive.
        The slowest fragment therefore follows the maximum of geometric
        attempt counts, rather than the mean count of one packet.
        """
        if ber <= 0 or not llr_enabled:
            return 0.0
        packets = self._packets_per_transfer(D, N, algo)
        packet_bytes = self._packet_bytes_per_transfer(D, N, algo)
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
            bdp = self._bdp(N, algo)
            window = cr if bdp == float('inf') else min(cr, bdp)
            window = min(window, packets)
            extra_rounds *= (window + 1.0) / 2.0
        return steps * T_serial_step * extra_rounds

    def _gbn_cascade(self, T_step, steps, D, N, algo, cr, ber, llr_enabled,
                     retry_mode='gobackn', retry_buffer_entries=None):
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
        schedule.
        """
        if ber <= 0 or not llr_enabled:
            return 0.0

        bits_per_chunk = self.chunk_size * 8
        P_chunk = 1 - (1 - ber) ** bits_per_chunk
        if P_chunk <= 0:
            return 0.0

        total_chunks = D * (N - 1) / N / self.chunk_size
        P_error_total = total_chunks * P_chunk

        bdp = self._bdp(N, algo)
        if bdp == float('inf'):
            window = cr
            cr_eff = cr
        else:
            window = min(cr, bdp)
            cr_eff = window

        window = min(window, self._packets_per_transfer(D, N, algo))
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
                               retry_mode, retry_buffer_entries):
        """Visible delay from reconstructing packets absent from the retry buffer."""
        if ber <= 0 or not llr_enabled or retry_buffer_entries is None:
            return 0.0

        required = self.required_retry_buffer_entries(D, N, algo)
        if required <= 0 or retry_buffer_entries >= required:
            return 0.0
        miss_fraction = 1.0 - max(0, retry_buffer_entries) / required

        p_chunk = 1.0 - (1.0 - ber) ** (self.chunk_size * 8)
        total_chunks = D * (N - 1) / N / self.chunk_size
        expected_errors = total_chunks * p_chunk

        bdp = self._bdp(N, algo)
        window = cr if bdp == float('inf') else min(cr, bdp)
        window = min(window, self._packets_per_transfer(D, N, algo))
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
                       retry_mode='gobackn', retry_buffer_entries=None):
        """Per-step delay variance from retransmission + queuing.

        Uses post-FEC BER when FEC is enabled (variance from corrected errors
        is near-zero; only uncorrectable errors trigger retransmission).

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
        effective_ber = (self._post_fec_ber(ber, fec_n, fec_k, fec_t)
                         if fec_enabled else ber)
        if effective_ber <= 0 and competing_bw <= 0:
            return 0.0

        steps = self._steps(N, algo)
        per_step = self._per_step(D, N, algo, steps)
        K_step = per_step / self.chunk_size

        bdp = self._bdp(N, algo)
        window = min(cr, bdp) if bdp != float('inf') else cr
        window = min(window, self._packets_per_transfer(D, N, algo))
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
                     strict_reliability=False, retry_limit=None):
        """Predict tail latency (pXX) in microseconds."""
        if competing_bw is None:
            competing_bw = mem_bw
        retry_buffer_entries = self._retry_buffer_capacity_entries(
            retry_buffer_entries, retry_buffer_bytes)
        mean = self.predict(
            data_bytes, num_gpus, algo, credits, ber=ber, mem_bw=mem_bw,
            startup_us=startup_us, fec_enabled=fec_enabled,
            llr_enabled=llr_enabled, competing_bw=competing_bw,
            fec_n=fec_n, fec_k=fec_k, fec_t=fec_t,
            retry_mode=retry_mode, retry_buffer_entries=retry_buffer_entries,
            optical_fraction=optical_fraction, optical_hops=optical_hops,
            strict_reliability=strict_reliability, retry_limit=retry_limit)
        if not math.isfinite(mean):
            return mean
        steps = self._steps(num_gpus, algo)
        T_step = self._step_time(self._per_step(data_bytes, num_gpus, algo, steps),
                                  num_gpus, algo)
        var_step = self._step_variance(T_step, data_bytes, num_gpus, algo,
                                        credits, ber, competing_bw, llr_enabled,
                                        fec_enabled, fec_n, fec_k, fec_t,
                                        retry_mode, retry_buffer_entries)

        if var_step <= 0:
            return mean

        effective_ber = (self._post_fec_ber(ber, fec_n, fec_k, fec_t)
                         if fec_enabled else ber)
        bits_per_chunk = self.chunk_size * 8
        P_chunk = 1 - (1 - effective_ber) ** bits_per_chunk if effective_ber > 0 else 0
        is_heavy_tail = P_chunk > 0.1

        if not is_heavy_tail:
            from scipy.stats import norm
            z = norm.ppf(percentile / 100)
            var_total = steps * var_step
            return mean + z * math.sqrt(var_total)
        else:
            bdp = self._bdp(num_gpus, algo)
            window = min(credits, bdp) if bdp != float('inf') else credits
            delay_per_error = window * self.chunk_size / self.BW_link
            lam = P_chunk / (1 - P_chunk) / delay_per_error if P_chunk < 1 else 1e6
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
                include_retransmission=True, include_contention=True):
        """Predict mean collective latency (µs).

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
            retry_buffer_entries, retry_buffer_bytes)
        optical_fraction = min(1.0, max(0.0, optical_fraction))
        S = startup_us if startup_us is not None else (
            self.S_nvls_startup if algo == 'nvls' else self.S_startup)
        steps = self._steps(num_gpus, algo)
        per_step = self._per_step(data_bytes, num_gpus, algo, steps)

        if algo == 'tree':
            log2N = max(1, int(math.log2(num_gpus)))
            T_fixed_step = self.T_min_switch
            T_serial_step = per_step / (self.tree_bw_per_level * log2N)
        elif algo == 'nvls':
            T_fixed_step = self.lat
            T_serial_step = (per_step * num_gpus) / self.nvls_bw
        else:
            T_fixed_step = self.lat
            T_serial_step = per_step / self.ring_bw

        Phi_cr = (self._credit_pressure(
            data_bytes, num_gpus, algo, credits) if include_credit else 1.0)
        Phi_fec = (
            self._fec_bw_overhead(fec_n, fec_k, optical_fraction)
            if fec_enabled and include_fec else 1.0)
        Phi_share = (
            self._shared_contention(
                algo, credits, competing_bw, data_bytes, num_gpus)
            if include_contention else 1.0)

        if strict_reliability and llr_enabled and retry_limit is not None:
            expected_losses = self.expected_permanent_losses(
                data_bytes, num_gpus, algo, ber, retry_limit,
                fec_enabled and include_fec, fec_n, fec_k, fec_t,
                optical_fraction, optical_hops)
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
                    optical_fraction, optical_hops)
            else:
                T_gbn_legacy = self._gbn_cascade(
                    T_step, steps, data_bytes, num_gpus, algo, credits,
                    effective_ber, llr_enabled, retry_mode,
                    retry_buffer_entries)
                sack_floor = self._retry_overhead(
                    T_serial_step, steps, data_bytes, num_gpus, algo, credits,
                    ber, llr_enabled, fec_enabled and include_fec,
                    fec_n, fec_k, fec_t, 'sack',
                    optical_fraction, optical_hops)
                bdp = self._bdp(num_gpus, algo)
                window = credits if bdp == float('inf') else min(credits, bdp)
                gbn_floor = sack_floor * (1.0 + 1.0 / max(window, 1.0))
                T_gbn = max(T_gbn_legacy, gbn_floor)
            T_reload = self._source_reload_penalty(
                data_bytes, num_gpus, algo, credits, effective_ber, llr_enabled,
                retry_mode, retry_buffer_entries)
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

def make_h200_ring_theory():
    """H200 profile used for ring, tree, and NVLS predictions."""
    return TheoryDerivedSurrogate(
        packet_bw_bytes_per_us=177000,
        ring_bw_bytes_per_us=94000,
        tree_bw_per_level_bytes_per_us=26250,
        nvls_bw_bytes_per_us=535500,
        link_latency_us=0.4,
        credit_bdp_ring_packets=91.5,
        credit_bdp_tree_packets=35.5,
        spray_chunk_bytes=131072,
        bulk_chunk_bytes=8 * 1024 * 1024,
        num_lanes=4,
        t_min_switch_us=1.1,
        startup_us=15.0,
    )


def make_h200_tree_theory():
    """H200 tree: NVSwitch bisection bandwidth and link credit pressure."""
    model = make_h200_ring_theory()
    model.num_lanes = 18
    return model


def make_h200_nvls_theory():
    """H200 NVLS: hardware multicast, no credit flow control."""
    return TheoryDerivedSurrogate(
        packet_bw_bytes_per_us=177000,
        ring_bw_bytes_per_us=94000,
        tree_bw_per_level_bytes_per_us=26250,
        nvls_bw_bytes_per_us=535500,
        link_latency_us=0.4,
        credit_bdp_ring_packets=91.5,
        credit_bdp_tree_packets=35.5,
        spray_chunk_bytes=131072,
        bulk_chunk_bytes=8 * 1024 * 1024,
        num_lanes=18,
        t_min_switch_us=1.1,
        startup_us=23.0,
        nvls_startup_us=23.0,
    )


def make_1024_optical_fattree_theory():
    """Profile for the 1024-GPU, 600-Gbps fat-tree optical study."""
    return TheoryDerivedSurrogate(
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


def make_surrogate(algo):
    """Factory by algorithm name."""
    if algo in ('ring', 'rin'):
        return make_h200_ring_theory()
    elif algo in ('tree', 'tre'):
        return make_h200_tree_theory()
    elif algo in ('nvls', 'nvl'):
        return make_h200_nvls_theory()
    else:
        return make_h200_ring_theory()
