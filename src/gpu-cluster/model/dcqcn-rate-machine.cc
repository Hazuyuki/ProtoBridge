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

#include "ns3/time-bounds.h"

namespace ns3
{

namespace
{
bool
IsUnrepresentableDelay(Time delay)
{
    return !TimeBounds::CanSchedule(delay);
}

Time
ClampedNanoSeconds(TimeBounds::Wide ns)
{
    return std::min(TimeBounds::FromNanoseconds(ns), TimeBounds::Horizon());
}
} // unnamed namespace

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
                          MakeTimeChecker(Time(0)))
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
                          MakeTimeChecker(Time(0)))
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
DcqcnRateMachine::DoDispose()
{
    NS_LOG_FUNCTION(this);
    // Recovery timers capture raw `this` (FastRecovery/AdditiveIncrease).
    // Unless every armed timer is cancelled here, dropping the last
    // reference lets the simulator fire into freed memory (codex round 12,
    // finding 3: OnCnp -> Dispose -> release -> Run crashed ASAN inside
    // FastRecovery).
    for (auto& kv : m_flows)
    {
        FlowState& s = kv.second;
        if (s.fastArmed)
        {
            Simulator::Cancel(s.fastTimer);
            s.fastArmed = false;
        }
        if (s.aiArmed)
        {
            Simulator::Cancel(s.aiTimer);
            s.aiArmed = false;
        }
    }
    Object::DoDispose();
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
    // Round 74 (codex finding 5): a degenerate interval at or beyond the
    // remaining horizon wraps the schedule to a negative deadline and the
    // timer fires immediately, shrinking alpha past the horizon — no timer
    // is armed for an unreachable interval.
    if (s.fastArmed)
    {
        Simulator::Cancel(s.fastTimer);
        s.fastArmed = false;
    }
    if (!IsUnrepresentableDelay(m_tFast))
    {
        s.fastTimer =
            TimeBounds::Schedule(m_tFast, &DcqcnRateMachine::FastRecovery, this, flowKey);
        s.fastArmed = true;
    }
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
        // Same horizon guard as OnCnp: an unreachable interval arms no timer.
        if (!IsUnrepresentableDelay(m_tFast))
        {
            s.fastTimer =
                TimeBounds::Schedule(m_tFast, &DcqcnRateMachine::FastRecovery, this, flowKey);
            s.fastArmed = true;
        }
    }
    else
    {
        // Fast cycles spent: hand off to additive increase until the floor.
        if (s.alpha > m_alphaFloor && !IsUnrepresentableDelay(m_tAi))
        {
            s.aiTimer =
                TimeBounds::Schedule(m_tAi, &DcqcnRateMachine::AdditiveIncrease, this, flowKey);
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

    if (s.alpha > m_alphaFloor && !IsUnrepresentableDelay(m_tAi))
    {
        s.aiTimer = TimeBounds::Schedule(m_tAi, &DcqcnRateMachine::AdditiveIncrease, this, flowKey);
        s.aiArmed = true;
    }
}

Time
DcqcnRateMachine::ShapingDelay(uint32_t chunkBytes, FlowKey flowKey)
{
    double rateBps = 0.0;
    if (m_maxRateBps > 0.0)
    {
        auto it = m_flows.find(flowKey);
        if (it != m_flows.end() && it->second.alpha > m_alphaFloor)
        {
            const double factor = std::clamp(1.0 - it->second.alpha / 2.0, 0.5, 1.0);
            rateBps = m_maxRateBps * factor;
        }
    }
    double ceilingBps;
    if (m_perQpCeilingGbps > 0.0 &&
        TimeBounds::TryBytesPerSecond(m_perQpCeilingGbps, ceilingBps))
    {
        rateBps = rateBps > 0.0 ? std::min(rateBps, ceilingBps) : ceilingBps;
    }
    if (rateBps <= 0.0)
    {
        return Time(0);
    }
    auto& state = GetOrCreate(flowKey);
    const auto now = TimeBounds::ToNanoseconds(Simulator::Now());
    const auto start = std::max(now, state.nextSendNs);
    const auto delay = start - now;
    const auto slot = TimeBounds::PacingSlot(chunkBytes, rateBps);
    const auto limit = TimeBounds::NanosecondLimit();
    state.nextSendNs = slot > limit - start ? limit : start + slot;
    return delay > 0 ? ClampedNanoSeconds(delay) : Time(0);
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
