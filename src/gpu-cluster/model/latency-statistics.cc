/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "latency-statistics.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/double.h"
#include "ns3/uinteger.h"
#include "ns3/random-variable-stream.h"

#include <algorithm>
#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("LatencyStatistics");

NS_OBJECT_ENSURE_REGISTERED(LatencyStatistics);

TypeId
LatencyStatistics::GetTypeId()
{
    static TypeId tid = TypeId("ns3::LatencyStatistics")
        .SetParent<Object>()
        .SetGroupName("GpuCluster")
        .AddConstructor<LatencyStatistics>()
        .AddAttribute("WarmupFraction",
                      "Fraction of simulation time to exclude as warmup (0.0 to 1.0)",
                      DoubleValue(0.2),
                      MakeDoubleAccessor(&LatencyStatistics::m_warmupFraction),
                      MakeDoubleChecker<double>(0.0, 1.0))
        .AddAttribute("SimulationDuration",
                      "Configured simulation stop time for warmup calculation",
                      TimeValue(Seconds(0)),
                      MakeTimeAccessor(&LatencyStatistics::SetSimulationDuration,
                                       &LatencyStatistics::GetSimulationDuration),
                      MakeTimeChecker())
        .AddAttribute("BootstrapResamples",
                      "Number of bootstrap resamples for confidence intervals",
                      UintegerValue(1000),
                      MakeUintegerAccessor(&LatencyStatistics::m_bootstrapResamples),
                      MakeUintegerChecker<uint32_t>(100))
        .AddTraceSource("FlowComplete",
                        "A flow completion was recorded",
                        MakeTraceSourceAccessor(&LatencyStatistics::m_flowCompleteTrace),
                        "ns3::LatencyStatistics::FlowCompleteTracedCallback");
    return tid;
}

LatencyStatistics::LatencyStatistics()
    : m_warmupFraction(0.2),
      m_simulationDuration(Seconds(0)),
      m_bootstrapResamples(1000)
{
    NS_LOG_FUNCTION(this);
}

LatencyStatistics::~LatencyStatistics()
{
    NS_LOG_FUNCTION(this);
}

void
LatencyStatistics::DoDispose()
{
    NS_LOG_FUNCTION(this);
    Clear();
    Object::DoDispose();
}

void
LatencyStatistics::RecordFlowStart(uint16_t flowId)
{
    NS_LOG_FUNCTION(this << flowId);
    m_flows[flowId].startTime = Simulator::Now();
}

void
LatencyStatistics::RecordFlowComplete(uint16_t flowId)
{
    NS_LOG_FUNCTION(this << flowId);

    auto it = m_flows.find(flowId);
    if (it == m_flows.end())
    {
        NS_LOG_WARN("Flow complete without start: flowId=" << flowId);
        return;
    }

    Time latency = Simulator::Now() - it->second.startTime;
    RecordLatency(flowId, latency);
}

void
LatencyStatistics::RecordLatency(uint16_t flowId, Time latency)
{
    NS_LOG_FUNCTION(this << flowId << latency);

    // Exclude warmup period using configured simulation duration
    // If duration is not set, use current simulation time as fallback
    Time now = Simulator::Now();
    Time simDuration = m_simulationDuration;
    if (simDuration == Seconds(0))
    {
        simDuration = now;
    }
    Time warmupEnd = Seconds(m_warmupFraction * simDuration.GetSeconds());
    if (m_warmupFraction > 0.0 && now <= warmupEnd)
    {
        NS_LOG_DEBUG("Sample in warmup period, excluding: flowId=" << flowId);
        return;
    }

    auto& record = m_flows[flowId];
    record.latenciesNs.push_back(latency.GetNanoSeconds());
    m_flowCompleteTrace(flowId, latency);
}

void
LatencyStatistics::SetWarmupFraction(double fraction)
{
    NS_LOG_FUNCTION(this << fraction);
    NS_ASSERT_MSG(fraction >= 0.0 && fraction < 1.0, "Warmup fraction must be in [0, 1)");
    m_warmupFraction = fraction;
}

double
LatencyStatistics::GetWarmupFraction() const
{
    return m_warmupFraction;
}

void
LatencyStatistics::SetBootstrapResamples(uint32_t resamples)
{
    m_bootstrapResamples = resamples;
}

uint32_t
LatencyStatistics::GetBootstrapResamples() const
{
    return m_bootstrapResamples;
}

double
LatencyStatistics::ComputePercentileValue(const std::vector<int64_t>& sortedSamples,
                                          double percentile) const
{
    if (sortedSamples.empty())
    {
        return 0.0;
    }

    double index = percentile * (sortedSamples.size() - 1);
    size_t lower = static_cast<size_t>(std::floor(index));
    size_t upper = static_cast<size_t>(std::ceil(index));

    if (lower == upper || upper >= sortedSamples.size())
    {
        return static_cast<double>(sortedSamples[std::min(lower, sortedSamples.size() - 1)]);
    }

    // Linear interpolation
    double frac = index - lower;
    return static_cast<double>(sortedSamples[lower]) * (1.0 - frac) +
           static_cast<double>(sortedSamples[upper]) * frac;
}

void
LatencyStatistics::BootstrapCi(const std::vector<int64_t>& samples, double percentile,
                                double& ciLow, double& ciHigh) const
{
    if (samples.size() < 10)
    {
        // Not enough samples for meaningful bootstrap
        double pVal = ComputePercentileValue(samples, percentile);
        ciLow = pVal;
        ciHigh = pVal;
        return;
    }

    Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();
    std::vector<double> bootstrapPercentiles;
    bootstrapPercentiles.reserve(m_bootstrapResamples);

    for (uint32_t b = 0; b < m_bootstrapResamples; b++)
    {
        std::vector<int64_t> resample;
        resample.reserve(samples.size());
        for (size_t i = 0; i < samples.size(); i++)
        {
            uint32_t idx = rng->GetInteger(0, samples.size() - 1);
            resample.push_back(samples[idx]);
        }
        std::sort(resample.begin(), resample.end());
        bootstrapPercentiles.push_back(ComputePercentileValue(resample, percentile));
    }

    std::sort(bootstrapPercentiles.begin(), bootstrapPercentiles.end());

    // 95% CI: 2.5th and 97.5th percentiles of bootstrap distribution
    size_t lowIdx = static_cast<size_t>(0.025 * bootstrapPercentiles.size());
    size_t highIdx = static_cast<size_t>(0.975 * bootstrapPercentiles.size());
    if (highIdx >= bootstrapPercentiles.size()) highIdx = bootstrapPercentiles.size() - 1;

    ciLow = bootstrapPercentiles[lowIdx];
    ciHigh = bootstrapPercentiles[highIdx];
}

LatencyStatistics::PercentileResult
LatencyStatistics::ComputePercentiles(uint16_t flowId) const
{
    PercentileResult result{};
    auto it = m_flows.find(flowId);
    if (it == m_flows.end() || it->second.latenciesNs.empty())
    {
        return result;
    }

    std::vector<int64_t> sorted = it->second.latenciesNs;
    std::sort(sorted.begin(), sorted.end());

    result.p50 = ComputePercentileValue(sorted, 0.50);
    result.p95 = ComputePercentileValue(sorted, 0.95);
    result.p99 = ComputePercentileValue(sorted, 0.99);
    result.sampleCount = sorted.size();

    BootstrapCi(sorted, 0.50, result.p50CiLow, result.p50CiHigh);
    BootstrapCi(sorted, 0.95, result.p95CiLow, result.p95CiHigh);
    BootstrapCi(sorted, 0.99, result.p99CiLow, result.p99CiHigh);

    return result;
}

LatencyStatistics::PercentileResult
LatencyStatistics::ComputeAggregatePercentiles() const
{
    PercentileResult result{};

    std::vector<int64_t> allLatencies;
    for (const auto& pair : m_flows)
    {
        allLatencies.insert(allLatencies.end(),
                            pair.second.latenciesNs.begin(),
                            pair.second.latenciesNs.end());
    }

    if (allLatencies.empty())
    {
        return result;
    }

    std::sort(allLatencies.begin(), allLatencies.end());

    result.p50 = ComputePercentileValue(allLatencies, 0.50);
    result.p95 = ComputePercentileValue(allLatencies, 0.95);
    result.p99 = ComputePercentileValue(allLatencies, 0.99);
    result.sampleCount = allLatencies.size();

    BootstrapCi(allLatencies, 0.50, result.p50CiLow, result.p50CiHigh);
    BootstrapCi(allLatencies, 0.95, result.p95CiLow, result.p95CiHigh);
    BootstrapCi(allLatencies, 0.99, result.p99CiLow, result.p99CiHigh);

    return result;
}

std::vector<uint16_t>
LatencyStatistics::GetFlowIds() const
{
    std::vector<uint16_t> ids;
    ids.reserve(m_flows.size());
    for (const auto& pair : m_flows)
    {
        if (!pair.second.latenciesNs.empty())
        {
            ids.push_back(pair.first);
        }
    }
    return ids;
}

uint32_t
LatencyStatistics::GetSampleCount(uint16_t flowId) const
{
    auto it = m_flows.find(flowId);
    if (it != m_flows.end())
    {
        return it->second.latenciesNs.size();
    }
    return 0;
}

void
LatencyStatistics::Clear()
{
    m_flows.clear();
}

void
LatencyStatistics::SetSimulationDuration(Time duration)
{
    m_simulationDuration = duration;
}

Time
LatencyStatistics::GetSimulationDuration() const
{
    return m_simulationDuration;
}

} // namespace ns3
