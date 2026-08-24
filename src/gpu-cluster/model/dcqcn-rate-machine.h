/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Per-flow DCQCN (Datacenter Quantized Congestion Notification) rate machine
 * for the scale-out (So) RoCEv2 fabric. The receive path hands Congestion
 * Notification Packets (CNP) to OnCnp(); the send path folds ShapingDelay()
 * into the egress chokepoint. DCQCN backs a per-flow Alpha rate factor off on
 * CNP and ramps it back via event-driven timers (no polling).
 */

#ifndef NS3_DCQCN_RATE_MACHINE_H
#define NS3_DCQCN_RATE_MACHINE_H

#include "ns3/object.h"
#include "ns3/nstime.h"
#include "ns3/event-id.h"

#include <unordered_map>

namespace ns3
{

/**
 * \ingroup gpucluster
 *
 * \brief Per-flow DCQCN rate machine for the So RoCEv2 fabric.
 *
 * OnCnp() grows a per-flow Alpha (the rate-backoff factor) and arms a
 * fast-recovery timer that multiplicatively shrinks Alpha; once the fast
 * cycles are spent, an additive-increase timer drives Alpha back to the
 * floor. ShapingDelay() maps Alpha onto an egress delay the send path folds
 * into its chokepoint: rateFactor = clamp(1 - alpha/2, 0.5, 1.0), and the
 * delay is the extra serialization time at the reduced rate. At Alpha = 0
 * (the default, before any CNP) ShapingDelay() is zero, so mounting the
 * machine is byte-identical to the unmounted baseline (parity-safe).
 *
 * The flow key is FabricEndpoint::MakeSeqKey(destRank, vcId, flowId): the key
 * the sender uses on the send path matches the key the CNP carries back, so
 * OnCnp and ShapingDelay address the same per-flow state.
 */
class DcqcnRateMachine : public Object
{
  public:
    static TypeId GetTypeId();

    DcqcnRateMachine();
    ~DcqcnRateMachine() override = default;

    using FlowKey = uint64_t;

    /**
     * \brief A CNP arrived for this flow. Grow Alpha (rate backoff) and arm
     *        fast recovery. Idempotent across rapid CNPs: re-arms the fast
     *        timer and resets the fast-cycle counter.
     */
    void OnCnp(FlowKey flowKey);

    /**
     * \brief Extra egress delay (ns) to shape a chunk of chunkBytes for this
     *        flow. Returns 0 when Alpha is at/below the floor (no congestion
     *        => no shaping), so the baseline is parity-identical.
     */
    Time ShapingDelay(uint32_t chunkBytes, FlowKey flowKey) const;

    /**
     * \brief Current Alpha for a flow (0 = no backoff, 1 = max backoff).
     *        Exposed for tests; production code uses ShapingDelay().
     */
    double GetAlpha(FlowKey flowKey) const;

  private:
    struct FlowState
    {
        double alpha{0.0};        ///< Current rate-backoff factor [0,1]
        double g{0.0};            ///< Current multiplicative step
        uint32_t fastCycles{0};    ///< Fast-recovery cycles consumed
        bool fastArmed{false};    ///< A fast-recovery timer is pending
        bool aiArmed{false};      ///< An additive-increase timer is pending
        EventId fastTimer;       ///< Pending fast-recovery event
        EventId aiTimer;         ///< Pending additive-increase event
    };

    FlowState& GetOrCreate(FlowKey flowKey);
    void FastRecovery(FlowKey flowKey);
    void AdditiveIncrease(FlowKey flowKey);
    void CancelTimers(FlowState& s);

    // TypeId Attributes
    Time m_tFast;                     ///< Fast-recovery timer interval
    uint32_t m_fastRecoveryMaxCycles; ///< Fast cycles before additive increase
    Time m_tAi;                       ///< Additive-increase timer interval
    double m_rai;                     ///< Additive Alpha decrement per AI tick
    double m_gInit;                   ///< Initial multiplicative step on first CNP
    double m_gDecay;                  ///< g *= gDecay per fast cycle
    double m_alphaFloor;              ///< Alpha floor (ShapingDelay==0 at/below)
    double m_maxRateBps;              ///< Max link rate (B/s) for shaping math

    std::unordered_map<FlowKey, FlowState> m_flows;
};

} // namespace ns3

#endif // NS3_DCQCN_RATE_MACHINE_H
