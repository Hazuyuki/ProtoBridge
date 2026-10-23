/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Per-flow RTTCC (RTT-based congestion control) rate machine implementation.
 */

#include "rttcc-rate-machine.h"

#include "ns3/double.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <limits>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("RttccRateMachine");

NS_OBJECT_ENSURE_REGISTERED(RttccRateMachine);

TypeId
RttccRateMachine::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::RttccRateMachine")
            .SetParent<Object>()
            .SetGroupName("GpuCluster")
            .AddConstructor<RttccRateMachine>()
            // RTT at/below which the path is considered uncongested. Must sit
            // above the base (zero-queue) RTT of the So path or RTTCC throttles
            // an idle fabric; scratch exposes it as --rttccTargetRttNs.
            .AddAttribute("TargetRtt",
                          "Target RTT: samples above back the rate off.",
                          TimeValue(NanoSeconds(10000)),
                          MakeTimeAccessor(&RttccRateMachine::m_targetRtt),
                          MakeTimeChecker())
            // Minimum spacing between RTT probes on one flow.
            .AddAttribute("ProbeInterval",
                          "Minimum spacing between RTT probes per flow.",
                          TimeValue(NanoSeconds(5000)),
                          MakeTimeAccessor(&RttccRateMachine::m_probeInterval),
                          MakeTimeChecker())
            // Additive rate increase per below-target sample.
            .AddAttribute("AdditiveIncreaseGbps",
                          "Rate increase per below-target RTT sample (Gbps).",
                          DoubleValue(10.0),
                          MakeDoubleAccessor(&RttccRateMachine::m_aiGbps),
                          MakeDoubleChecker<double>(0.0))
            // Multiplicative-decrease scale: rate *= 1 - beta*(rtt-target)/rtt.
            .AddAttribute("Beta",
                          "Multiplicative-decrease scale on over-target RTT.",
                          DoubleValue(0.8),
                          MakeDoubleAccessor(&RttccRateMachine::m_beta),
                          MakeDoubleChecker<double>(0.0, 1.0))
            // Rate floor.
            .AddAttribute("MinRateGbps",
                          "Rate floor (Gbps).",
                          DoubleValue(1.0),
                          MakeDoubleAccessor(&RttccRateMachine::m_minRateGbps),
                          MakeDoubleChecker<double>(0.0))
            // Line rate: the initial and maximum flow rate.
            .AddAttribute("LineRateGbps",
                          "Line rate (Gbps): initial and maximum flow rate.",
                          DoubleValue(400.0),
                          MakeDoubleAccessor(&RttccRateMachine::m_lineRateGbps),
                          MakeDoubleChecker<double>(0.0));
    return tid;
}

RttccRateMachine::RttccRateMachine()
    : m_targetRtt(NanoSeconds(10000)),
      m_probeInterval(NanoSeconds(5000)),
      m_aiGbps(10.0),
      m_beta(0.8),
      m_minRateGbps(1.0),
      m_lineRateGbps(400.0),
      m_minRttNs(std::numeric_limits<int64_t>::max())
{
    NS_LOG_FUNCTION(this);
}

RttccRateMachine::FlowState&
RttccRateMachine::GetOrCreate(FlowKey flowKey)
{
    return m_flows[flowKey];
}

double
RttccRateMachine::LineRateBps() const
{
    // 1 Gbps = 1.25e8 B/s (same convention as DcqcnRateMachine).
    return m_lineRateGbps * 1.25e8;
}

bool
RttccRateMachine::MaybeStartProbe(FlowKey flowKey, uint32_t& probeSeq)
{
    FlowState& s = GetOrCreate(flowKey);
    int64_t nowNs = Simulator::Now().GetNanoSeconds();
    if (s.lastProbeNs >= 0 && nowNs - s.lastProbeNs < m_probeInterval.GetNanoSeconds())
    {
        return false;
    }
    s.lastProbeNs = nowNs;
    probeSeq = s.nextProbeSeq++;
    // Bound the in-flight table: probes ride the control path (never
    // tail-dropped, BER-bypassed), so entries only linger after a rare wire
    // loss of the probe or its response. Cap defensively.
    if (s.inflightProbes.size() > 1024)
    {
        s.inflightProbes.clear();
    }
    s.inflightProbes[probeSeq] = nowNs;
    ++m_probesSent;
    NS_LOG_DEBUG("RTT probe seq=" << probeSeq << " flow=" << flowKey);
    return true;
}

void
RttccRateMachine::OnProbeResponse(FlowKey flowKey, uint32_t probeSeq)
{
    auto fit = m_flows.find(flowKey);
    if (fit == m_flows.end())
    {
        return; // response for a flow this machine never probed
    }
    FlowState& s = fit->second;
    auto pit = s.inflightProbes.find(probeSeq);
    if (pit == s.inflightProbes.end())
    {
        return; // stale or duplicate echo
    }
    int64_t rttNs = Simulator::Now().GetNanoSeconds() - pit->second;
    s.inflightProbes.erase(pit);

    ++m_rttSamples;
    m_minRttNs = std::min(m_minRttNs, rttNs);
    m_maxRttNs = std::max(m_maxRttNs, rttNs);

    double lineBps = LineRateBps();
    double rate = (s.rateBps > 0.0) ? s.rateBps : lineBps;
    int64_t targetNs = m_targetRtt.GetNanoSeconds();
    if (rttNs <= targetNs)
    {
        // Below target: additive increase toward the line rate.
        rate = std::min(lineBps, rate + m_aiGbps * 1.25e8);
    }
    else if (rttNs > 0)
    {
        // Over target: multiplicative decrease proportional to the overshoot.
        // (rtt-target)/rtt < 1, so the factor stays within (1-beta, 1).
        double factor =
            1.0 - m_beta * static_cast<double>(rttNs - targetNs) / static_cast<double>(rttNs);
        rate = std::max(m_minRateGbps * 1.25e8, rate * factor);
    }
    // Store 0 for "at line rate" so ShapingDelay stays a no-op there.
    s.rateBps = (rate >= lineBps) ? 0.0 : rate;
    NS_LOG_DEBUG("RTT sample " << rttNs << "ns flow=" << flowKey
                               << " rate=" << GetRateGbps(flowKey) << "Gbps");
}

Time
RttccRateMachine::ShapingDelay(uint32_t chunkBytes, FlowKey flowKey)
{
    auto it = m_flows.find(flowKey);
    if (it == m_flows.end() || it->second.rateBps <= 0.0)
    {
        // Unknown flow or at line rate: un-paced (the NetDevice serializes at
        // the line rate anyway), byte-identical to the unmounted baseline.
        return Time(0);
    }
    FlowState& s = it->second;
    int64_t nowNs = Simulator::Now().GetNanoSeconds();
    int64_t delayNs = (s.nextSendNs > nowNs) ? (s.nextSendNs - nowNs) : 0;
    // The rate slot this chunk occupies (ns); the next chunk may egress one
    // slot after this chunk's egress start (same pacer as the per-QP ceiling).
    double slotSec = static_cast<double>(chunkBytes) / s.rateBps;
    int64_t slotNs = std::max<int64_t>(1, static_cast<int64_t>(slotSec * 1e9));
    s.nextSendNs = (nowNs + delayNs) + slotNs;
    return (delayNs > 0) ? NanoSeconds(delayNs) : Time(0);
}

double
RttccRateMachine::GetRateGbps(FlowKey flowKey) const
{
    auto it = m_flows.find(flowKey);
    if (it == m_flows.end() || it->second.rateBps <= 0.0)
    {
        return m_lineRateGbps;
    }
    return it->second.rateBps / 1.25e8;
}

uint64_t
RttccRateMachine::GetProbesSent() const
{
    return m_probesSent;
}

uint64_t
RttccRateMachine::GetRttSamples() const
{
    return m_rttSamples;
}

int64_t
RttccRateMachine::GetMinRttNs() const
{
    return m_minRttNs;
}

int64_t
RttccRateMachine::GetMaxRttNs() const
{
    return m_maxRttNs;
}

double
RttccRateMachine::GetMinFlowRateGbps() const
{
    double minRate = m_lineRateGbps;
    for (const auto& kv : m_flows)
    {
        if (kv.second.rateBps > 0.0)
        {
            minRate = std::min(minRate, kv.second.rateBps / 1.25e8);
        }
    }
    return minRate;
}

} // namespace ns3
