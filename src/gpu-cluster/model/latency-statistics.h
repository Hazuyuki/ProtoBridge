/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Per-flow latency statistics with percentile computation and bootstrap CIs
 */

#ifndef LATENCY_STATISTICS_H
#define LATENCY_STATISTICS_H

#include "ns3/object.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

#include <vector>
#include <unordered_map>
#include <cstdint>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Per-flow latency statistics collector
 *
 * Collects flow completion times and computes:
 * - P50, P95, P99 latencies per flow
 * - Bootstrap 95% confidence intervals for each percentile
 * - Warmup period exclusion (first 20% of simulation time by default)
 *
 * Usage:
 * 1. Record flow start times via RecordFlowStart(flowId)
 * 2. Record flow completions via RecordFlowComplete(flowId)
 * 3. After simulation, call ComputeStatistics() to get percentile results
 */
class LatencyStatistics : public Object
{
  public:
    static TypeId GetTypeId();

    LatencyStatistics();
    ~LatencyStatistics() override;

    /**
     * @brief Record the start time of a flow
     * @param flowId Flow identifier
     */
    void RecordFlowStart(uint16_t flowId);

    /**
     * @brief Record the completion of a flow
     * @param flowId Flow identifier
     */
    void RecordFlowComplete(uint16_t flowId);

    /**
     * @brief Record a raw latency sample directly
     * @param flowId Flow identifier
     * @param latency Flow completion latency
     */
    void RecordLatency(uint16_t flowId, Time latency);

    /**
     * @brief Set the warmup fraction (0.0 to 1.0)
     * Samples recorded during the first warmupFraction * simTime are excluded
     */
    void SetWarmupFraction(double fraction);
    double GetWarmupFraction() const;

    void SetSimulationDuration(Time duration);
    Time GetSimulationDuration() const;

    /**
     * @brief Set number of bootstrap resamples for CI
     */
    void SetBootstrapResamples(uint32_t resamples);
    uint32_t GetBootstrapResamples() const;

    /**
     * @brief Result of percentile computation
     */
    struct PercentileResult
    {
        double p50;
        double p95;
        double p99;
        double p50CiLow;    ///< Bootstrap 95% CI lower bound for P50
        double p50CiHigh;   ///< Bootstrap 95% CI upper bound for P50
        double p95CiLow;
        double p95CiHigh;
        double p99CiLow;
        double p99CiHigh;
        uint32_t sampleCount;
    };

    /**
     * @brief Compute percentile statistics for a specific flow
     * @param flowId Flow identifier
     * @return Percentile results (all zeros if no samples)
     */
    PercentileResult ComputePercentiles(uint16_t flowId) const;

    /**
     * @brief Compute aggregate percentile statistics across all flows
     * @return Percentile results
     */
    PercentileResult ComputeAggregatePercentiles() const;

    /**
     * @brief Get all flow IDs that have recorded samples
     */
    std::vector<uint16_t> GetFlowIds() const;

    /**
     * @brief Get number of samples for a specific flow
     */
    uint32_t GetSampleCount(uint16_t flowId) const;

    /**
     * @brief Clear all recorded data
     */
    void Clear();

    typedef TracedCallback<uint16_t, Time> FlowCompleteTracedCallback; ///< flowId, latency

  private:
    void DoDispose() override;

    /**
     * @brief Compute percentile from a sorted sample vector
     * @param sortedSamples Sorted vector of latency values in nanoseconds
     * @param percentile Percentile (0.0 to 1.0)
     * @return Percentile value in nanoseconds
     */
    double ComputePercentileValue(const std::vector<int64_t>& sortedSamples, double percentile) const;

    /**
     * @brief Bootstrap CI computation
     * @param samples Raw latency samples in nanoseconds
     * @param percentile Target percentile
     * @param ciLow Output: lower bound of 95% CI
     * @param ciHigh Output: upper bound of 95% CI
     */
    void BootstrapCi(const std::vector<int64_t>& samples, double percentile,
                     double& ciLow, double& ciHigh) const;

    struct FlowRecord
    {
        Time startTime;
        std::vector<int64_t> latenciesNs;  ///< Completed flow latencies in nanoseconds
    };

    std::unordered_map<uint16_t, FlowRecord> m_flows;
    double m_warmupFraction;
    Time m_simulationDuration;
    uint32_t m_bootstrapResamples;

    TracedCallback<uint16_t, Time> m_flowCompleteTrace;
};

} // namespace ns3

#endif /* LATENCY_STATISTICS_H */
