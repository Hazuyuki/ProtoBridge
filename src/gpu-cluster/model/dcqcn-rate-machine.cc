/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Per-flow DCQCN rate machine implementation.
 */

#include "dcqcn-rate-machine.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/double.h"
#include "ns3/uinteger.h"
#include "ns3/enum.h"

#include <algorithm>
#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("DcqcnRateMachine");

NS_OBJECT_ENSURE_REGISTERED(DcqcnRateMachine);

TypeId
DcqcnRateMachine::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::DcqcnRateMachine")
            .SetParent<Object>()
            .SetGroupName("GpuCluster")
            .AddConstructor<DcqcnRateMachine>()
            // Fast-recovery timer: multiplicative Alpha shrinkage per tick.
            .AddAttribute("TFast",
                          "Fast-recovery timer interval.",
                          TimeValue(NanoSeconds(50000)),
                          MakeTimeAccessor(&DcqcnRateMachine::m_tFast),
                          MakeTimeChecker())
            // Number of fast cycles before additive increase takes over.
            .AddAttribute("FastRecoveryMaxCycles",
                          "Fast-recovery cycles before additive increase.",
                          UintegerValue(5),
                          MakeUintegerAccessor(&DcqcnRateMachine::m_fastRecoveryMaxCycles),
                          MakeUintegerChecker<uint32_t>())
            // Additive-increase timer: linear Alpha decrement per tick.
            .AddAttribute("TAi",
                          "Additive-increase timer interval.",
                          TimeValue(NanoSeconds(100000)),
                          MakeTimeAccessor(&DcqcnRateMachine::m_tAi),
                          MakeTimeChecker())
            // Additive Alpha decrement per AI tick.
            .AddAttribute("RAI",
                          "Additive Alpha decrement per AI tick.",
                          DoubleValue(0.01),
                          MakeDoubleAccessor(&DcqcnRateMachine::m_rai),
                          MakeDoubleChecker<double>(0.0))
            // Initial multiplicative step on the first CNP of a burst.
            .AddAttribute("GInit",
                          "Initial multiplicative step g on first CNP.",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&DcqcnRateMachine::m_gInit),
                          MakeDoubleChecker<double>(0.0, 1.0))
            // g *= gDecay per fast cycle (steps shrink toward recovery).
            .AddAttribute("GDecay",
                          "Multiplicative decay of g per fast cycle.",
                          DoubleValue(0.5),
                          MakeDoubleAccessor(&DcqcnRateMachine::m_gDecay),
                          MakeDoubleChecker<double>(0.0, 1.0))
            // Alpha floor: ShapingDelay==0 at/below this.
            .AddAttribute("AlphaFloor",
                          "Alpha floor (no shaping at/below).",
                          DoubleValue(0.0),
                          MakeDoubleAccessor(&DcqcnRateMachine::m_alphaFloor),
                          MakeDoubleChecker<double>(0.0, 1.0))
            // Max link rate in bytes/sec for shaping-delay math.
            .AddAttribute("MaxRateBps",
                          "Max link rate (B/s) for shaping-delay math.",
                          DoubleValue(50.0e9),
                          MakeDoubleAccessor(&DcqcnRateMachine::m_maxRateBps),
                          MakeDoubleChecker<double>(0.0))
            // Opt-in per-QP sustained-throughput ceiling (Gbps; 0 = off). A
            // HARDWARE per-QP NIC cap (e.g. ~49 Gbps single-QP ib_write_bw),
            // independent of congestion. Enforced as a true per-flow pacer.
            .AddAttribute("PerQpCeilingGbps",
                          "Per-QP sustained throughput ceiling (Gbps, 0=off).",
                          DoubleValue(0.0),
                          MakeDoubleAccessor(&DcqcnRateMachine::m_perQpCeilingGbps),
                          MakeDoubleChecker<double>(0.0));
    return tid;
}

DcqcnRateMachine::DcqcnRateMachine()
    : m_tFast(NanoSeconds(50000)),
      m_fastRecoveryMaxCycles(5),
      m_tAi(NanoSeconds(100000)),
      m_rai(0.01),
      m_gInit(0.5),
      m_gDecay(0.5),
      m_alphaFloor(0.0),
      m_maxRateBps(50.0e9),
      m_perQpCeilingGbps(0.0)
{
    NS_LOG_FUNCTION(this);
}

DcqcnRateMachine::FlowState&
DcqcnRateMachine::GetOrCreate(FlowKey flowKey)
{
    return m_flows[flowKey];
}

void
DcqcnRateMachine::OnCnp(FlowKey flowKey)
{
    NS_LOG_FUNCTION(this << flowKey);
    FlowState& s = GetOrCreate(flowKey);

    // A fresh CNP (re)enters fast recovery: reset g to the initial step and
    // the fast-cycle counter. Cancel any pending additive-increase timer.
    if (!s.fastArmed && !s.aiArmed)
    {
        s.g = m_gInit; // first CNP of a burst
    }
    else if (s.aiArmed)
    {
        // Coming back from additive increase: restart fast recovery fresh.
        s.g = m_gInit;
    }
    s.fastCycles = 0;
    if (s.aiArmed)
    {
        Simulator::Cancel(s.aiTimer);
        s.aiArmed = false;
    }

    // Backoff: alpha += (1 - alpha) * g, clamped to [0,1].
    s.alpha = s.alpha + (1.0 - s.alpha) * s.g;
    if (s.alpha > 1.0)
    {
        s.alpha = 1.0;
    }
    if (s.alpha < 0.0)
    {
        s.alpha = 0.0;
    }

    // Arm fast recovery (re-arm if already pending so the timer resets).
    if (s.fastArmed)
    {
        Simulator::Cancel(s.fastTimer);
    }
    s.fastTimer = Simulator::Schedule(m_tFast, &DcqcnRateMachine::FastRecovery, this, flowKey);
    s.fastArmed = true;
}

void
DcqcnRateMachine::FastRecovery(FlowKey flowKey)
{
    NS_LOG_FUNCTION(this << flowKey);
    FlowState& s = GetOrCreate(flowKey);
    s.fastArmed = false;

    ++s.fastCycles;
    // Multiplicative increase of rate (backoff shrinks): alpha -= alpha * g.
    s.g *= m_gDecay;
    s.alpha = s.alpha - s.alpha * s.g;
    if (s.alpha < m_alphaFloor)
    {
        s.alpha = m_alphaFloor;
    }

    if (s.fastCycles < m_fastRecoveryMaxCycles)
    {
        s.fastTimer = Simulator::Schedule(m_tFast, &DcqcnRateMachine::FastRecovery, this, flowKey);
        s.fastArmed = true;
    }
    else
    {
        // Fast cycles spent: hand off to additive increase until the floor.
        if (s.alpha > m_alphaFloor)
        {
            s.aiTimer =
                Simulator::Schedule(m_tAi, &DcqcnRateMachine::AdditiveIncrease, this, flowKey);
            s.aiArmed = true;
        }
    }
}

void
DcqcnRateMachine::AdditiveIncrease(FlowKey flowKey)
{
    NS_LOG_FUNCTION(this << flowKey);
    FlowState& s = GetOrCreate(flowKey);
    s.aiArmed = false;

    s.alpha -= m_rai;
    if (s.alpha < m_alphaFloor)
    {
        s.alpha = m_alphaFloor;
    }

    if (s.alpha > m_alphaFloor)
    {
        s.aiTimer = Simulator::Schedule(m_tAi, &DcqcnRateMachine::AdditiveIncrease, this, flowKey);
        s.aiArmed = true;
    }
}

Time
DcqcnRateMachine::ShapingDelay(uint32_t chunkBytes, FlowKey flowKey)
{
    // DCQCN congestion backoff: read-only, never creates flow state (so the
    // ceiling=0 baseline is byte-identical to the pre-pacer machine). At
    // alpha=0 (no CNP) this is zero.
    double dcqcnSec = 0.0;
    if (m_maxRateBps > 0.0)
    {
        auto it = m_flows.find(flowKey);
        if (it != m_flows.end() && it->second.alpha > m_alphaFloor)
        {
            double alpha = it->second.alpha;
            // rateFactor = clamp(1 - alpha/2, 0.5, 1.0): alpha=0 -> 1.0 (no
            // delay); alpha=1 -> 0.5 (double the serialization time).
            double rateFactor = std::max(0.5, std::min(1.0, 1.0 - alpha / 2.0));
            // base serialization time at max rate; extra delay so the
            // chunk experiences the reduced rate.
            double baseSeconds = static_cast<double>(chunkBytes) / m_maxRateBps;
            double d = baseSeconds * (1.0 / rateFactor - 1.0);
            if (d > 0.0)
            {
                dcqcnSec = d;
            }
        }
    }

    // Per-QP ceiling: a TRUE per-flow rate pacer. Spaces consecutive chunks of
    // the same flow (one QP) so the aggregate rate settles to the ceiling. A
    // constant per-chunk offset would not cap sustained throughput -- the
    // NetDevice serializes a back-to-back burst at the link rate; only pacing
    // the next chunk's earliest egress time does. Only mutates state when the
    // ceiling is on (> 0), so the off baseline is parity-identical.
    double paceSec = 0.0;
    if (m_perQpCeilingGbps > 0.0)
    {
        // 1 Gbps = 1e9 bit/s = 1.25e8 B/s (matches MaxRateBps 50e9 == 400 G).
        double ceilingBps = m_perQpCeilingGbps * 1.25e8;
        FlowState& s = GetOrCreate(flowKey);
        int64_t nowNs = Simulator::Now().GetNanoSeconds();
        // Earliest this chunk may egress (ns); 0 before the flow's first chunk.
        int64_t delayNs = (s.nextSendNs > nowNs) ? (s.nextSendNs - nowNs) : 0;
        // The ceiling-rate slot this chunk occupies (ns).
        double slotSec = static_cast<double>(chunkBytes) / ceilingBps;
        int64_t slotNs = static_cast<int64_t>(slotSec * 1e9);
        if (slotNs < 1)
        {
            slotNs = 1; // avoid a zero-slot spin
        }
        // The next chunk on this flow may egress one ceiling slot after
        // this chunk's egress start (nowNs + delayNs).
        s.nextSendNs = (nowNs + delayNs) + slotNs;
        paceSec = static_cast<double>(delayNs) / 1e9;
    }

    if (dcqcnSec <= 0.0 && paceSec <= 0.0)
    {
        return Time(0);
    }
    return Time::FromDouble(std::max(dcqcnSec, paceSec), Time::S);
}

double
DcqcnRateMachine::GetAlpha(FlowKey flowKey) const
{
    auto it = m_flows.find(flowKey);
    if (it == m_flows.end())
    {
        return 0.0;
    }
    return it->second.alpha;
}

} // namespace ns3
