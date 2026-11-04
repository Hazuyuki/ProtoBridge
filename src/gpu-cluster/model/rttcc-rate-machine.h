/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Per-flow RTTCC (RTT-based congestion control) rate machine for the
 * scale-out (So) fabric. Unlike DCQCN (which needs the lossless RoCE
 * ECN/CNP loop), RTTCC senses congestion from round-trip-time probes, so
 * it works on the plain lossy Ethernet fabric where no ECN marking exists
 * (NVIDIA RTT-CC / Swift-style delay-based control).
 */

#ifndef NS3_RTTCC_RATE_MACHINE_H
#define NS3_RTTCC_RATE_MACHINE_H

#include "ns3/object.h"
#include "ns3/nstime.h"

#include <cstdint>
#include <unordered_map>

namespace ns3
{

/**
 * \ingroup gpucluster
 *
 * \brief Per-flow RTT-probe-based congestion control (RTTCC) for So fabrics.
 *
 * The send path asks MaybeStartProbe() at the egress chokepoint: at most one
 * RTT_PROBE per ProbeInterval per flow accompanies the data stream (the
 * endpoint emits the probe packet; this class only keeps the send timestamp
 * keyed by probe sequence — timestamps never go on the wire). The receiver
 * reflects each probe as an RTT_RESPONSE; OnProbeResponse() turns the echo
 * into an RTT sample and adjusts the flow rate, Swift-style:
 *
 *   rtt <= TargetRtt : rate += AdditiveIncreaseGbps  (capped at LineRateGbps)
 *   rtt >  TargetRtt : rate *= 1 - Beta*(rtt-Target)/rtt  (floored at MinRateGbps)
 *
 * ShapingDelay() enforces the rate as a TRUE per-flow pacer (per-flow
 * nextSendNs, the same mechanism as DcqcnRateMachine's per-QP ceiling): it
 * spaces consecutive chunks so the flow's aggregate rate settles to the
 * current rate. While a flow's rate sits at the line rate (the initial state,
 * before any over-target RTT sample) ShapingDelay() returns 0 and mutates
 * nothing, so an uncongested flow is byte-identical to the unmounted
 * baseline; the machine itself is only mounted under --rttcc (default off).
 *
 * The flow key is FabricEndpoint::MakeRateFlowKey(sourceRank, destRank, vcId,
 * flowId). The key carries the sender rank (codex round 23, finding 1 — the
 * earlier MakeSeqKey(dest, vc, flow) form documented here had no source
 * rank, and following the stale doc updated a key no send path reads), so a
 * single machine may serve multiple senders through per-sender state, though
 * the sim still mounts one machine per endpoint.
 */
class RttccRateMachine : public Object
{
  public:
    static TypeId GetTypeId();

    RttccRateMachine();
    ~RttccRateMachine() override = default;

    using FlowKey = uint64_t;

    /**
     * \brief Ask whether an RTT probe should accompany the data chunk being
     *        sent on this flow now. True at most once per ProbeInterval per
     *        flow; on true, probeSeq is the sequence number to stamp on the
     *        RTT_PROBE and the send timestamp is recorded internally.
     */
    bool MaybeStartProbe(FlowKey flowKey, uint32_t& probeSeq);

    /**
     * \brief An RTT_RESPONSE echoing probeSeq arrived for this flow. Computes
     *        the RTT sample and applies the Swift-style rate update. Unknown
     *        or duplicate probe sequences are ignored.
     */
    void OnProbeResponse(FlowKey flowKey, uint32_t probeSeq);

    /**
     * \brief Pacer delay (ns) before this chunk may egress so the flow's
     *        sustained rate matches the RTTCC rate. Returns 0 and mutates
     *        nothing while the flow rate is at the line rate (uncongested).
     */
    Time ShapingDelay(uint32_t chunkBytes, FlowKey flowKey);

    /// Current rate (Gbps) of a flow; LineRateGbps if the flow is unknown.
    double GetRateGbps(FlowKey flowKey) const;

    // Aggregate stats for the RESULT block / tests.
    uint64_t GetProbesSent() const;
    uint64_t GetRttSamples() const;
    int64_t GetMinRttNs() const;  ///< INT64_MAX before the first sample
    int64_t GetMaxRttNs() const;  ///< 0 before the first sample
    /// Minimum current rate across known flows (Gbps); LineRateGbps if none.
    double GetMinFlowRateGbps() const;

  private:
    struct FlowState
    {
        double rateBps{0.0};      ///< Current rate (B/s); 0 = at line rate
        int64_t nextSendNs{0};    ///< Earliest next-chunk egress (pacer), ns
        int64_t lastProbeNs{-1};  ///< Last probe send time (-1 = never)
        uint32_t nextProbeSeq{0}; ///< Next probe sequence number
        std::unordered_map<uint32_t, int64_t> inflightProbes; ///< seq -> sentNs
    };

    FlowState& GetOrCreate(FlowKey flowKey);
    double LineRateBps() const;

    // TypeId Attributes
    Time m_targetRtt;         ///< Target RTT: above = congested, below = ramp
    Time m_probeInterval;     ///< Min spacing between probes on one flow
    double m_aiGbps;          ///< Additive rate increase per below-target sample
    double m_beta;            ///< Multiplicative-decrease scale (Swift beta)
    double m_minRateGbps;     ///< Rate floor
    double m_lineRateGbps;    ///< Line rate (initial + max rate)

    // Aggregate stats
    uint64_t m_probesSent{0};
    uint64_t m_rttSamples{0};
    int64_t m_minRttNs;
    int64_t m_maxRttNs{0};

    std::unordered_map<FlowKey, FlowState> m_flows;
};

} // namespace ns3

#endif // NS3_RTTCC_RATE_MACHINE_H
