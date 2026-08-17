/*
 * SPDX-License-Identifier: GPL-2.0-only
 * hierarchical-reducescatter.h
 *
 * Hierarchical ReduceScatter for multi-node GPU clusters.
 * 2-phase algorithm:
 * 1. Local reduce-scatter (NVLink/MetaXLink within node)
 * 2. Inter-node reduce-scatter (RDMA across nodes)
 * 3. Complete
 */

#ifndef HIERARCHICAL_REDUCESCATTER_H
#define HIERARCHICAL_REDUCESCATTER_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>
#include <unordered_map>

namespace ns3
{

enum class HierReduceScatterPhase
{
    LOCAL_REDUCESCATTER,
    INTER_REDUCESCATTER,
    COMPLETED
};

enum class HierReduceScatterState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class HierarchicalReduceScatter : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    HierarchicalReduceScatter();
    virtual ~HierarchicalReduceScatter();

    // CollectiveInjector overrides
    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    // Multi-node configuration
    void SetGpusPerNode(uint32_t gpusPerNode);
    void SetNumNodes(uint32_t numNodes);
    void SetIntraNodeAlgorithm(const std::string& algo);
    void SetInterNodeStartupDelay(Time delay);

    // Protocol-aware inter-node startup delays: separate LL/LL128/SIMPLE values
    // for inter-node phase transitions. Unlike global GetPhaseStartupDelayNs()
    // which adds perGpu*numGpus barrier, inter-node startup only needs protocol
    // delay since GPUs are already synchronized after local phases.
    void SetInterNodeStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetInterNodeStartupDelayNs(uint64_t chunkSize) const;

    // Protocol-aware local phase startup delays: models persistent kernel wakeup.
    void SetLocalStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetLocalStartupDelayNs(uint64_t chunkSize) const;
    void SetStartupDelays(Time ll, Time ll128, Time simple);
    void SetStartupPerGpuNs(uint64_t perGpuNs);
    void SetLlThreshold(uint64_t threshold);
    void SetLl128Threshold(uint64_t threshold);

    // Reduce compute delays per phase (models reduction computation time)
    void SetLocalComputeDelay(Time delay);
    void SetInterNodeComputeDelay(Time delay);

    // Protocol-aware + data-proportional compute delays:
    // computeDelay = baseDelay(chunkSize) + perByteNs * chunkSize
    void SetLocalComputePerByteNs(uint64_t perByteNs);
    void SetInterNodeComputePerByteNs(uint64_t perByteNs);
    void SetLocalComputeBaseDelays(Time ll, Time ll128, Time simple);
    void SetInterNodeComputeBaseDelays(Time ll, Time ll128, Time simple);
    uint64_t GetLocalComputeDelayNs(uint64_t chunkSize) const;
    uint64_t GetInterNodeComputeDelayNs(uint64_t chunkSize) const;

    // Inter-node BW ramp-up delay (models RDMA pipeline ramp-up)
    void SetInterNodeBwRamp(Time rampDelay, uint64_t rampThresholdBytes);
    uint64_t GetInterNodeRampDelayNs(uint64_t chunkSize) const;

    // Fullmesh per-step delay: models step-by-step pipeline overhead within
    // fullmesh local phases. Uses the same LL/LL128/SIMPLE protocol thresholds.
    void SetFullmeshPerStepStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetFullmeshStepDelayNs(uint64_t chunkSize) const;

    HierReduceScatterState GetState() const { return m_state; }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);

    // Phase management
    void StartLocalReduceScatter();
    void OnLocalReduceScatterChunkReceived(uint16_t destRank, uint32_t size);
    void CheckLocalReduceScatterComplete(uint16_t gpu);
    void StartInterNodeReduceScatter();
    void OnInterNodeReduceScatterChunkReceived(uint16_t destRank, uint32_t size);
    void CheckInterNodeReduceScatterComplete(uint16_t gpu);
    void Complete();

    // Helpers
    uint32_t GetNodeId(uint16_t rank) const;
    uint16_t GetLocalIdx(uint16_t globalRank) const;
    uint16_t GetGlobalRank(uint32_t nodeId, uint16_t localIdx) const;
    uint64_t GetPhaseStartupDelayNs(uint64_t chunkSize) const;

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint16_t m_flowId;
    uint8_t m_protocolId;

    uint32_t m_gpusPerNode;
    uint32_t m_numNodes;
    std::string m_intraNodeAlgorithm;

    // Data sizes for each phase
    uint64_t m_segmentSize;    // dataSize / gpusPerNode (local reduce-scatter result)
    uint64_t m_interNodeChunk; // dataSize / totalGpus (inter-node result per GPU)

    HierReduceScatterState m_state;
    HierReduceScatterPhase m_currentPhase;
    uint64_t m_startupDelayNs;
    uint64_t m_interNodeStartupDelayNs;
    uint64_t m_interNodeStartupLLNs;      // Protocol-aware inter-node startup for LL
    uint64_t m_interNodeStartupLL128Ns;   // Protocol-aware inter-node startup for LL128
    uint64_t m_interNodeStartupSIMPLENs;  // Protocol-aware inter-node startup for SIMPLE
    uint64_t m_localStartupLLNs;         // Protocol-aware local phase startup for LL
    uint64_t m_localStartupLL128Ns;      // Protocol-aware local phase startup for LL128
    uint64_t m_localStartupSIMPLENs;     // Protocol-aware local phase startup for SIMPLE
    uint64_t m_startupLLNs;
    uint64_t m_startupLL128Ns;
    uint64_t m_startupSIMPLENs;
    uint64_t m_startupPerGpuNs;
    uint64_t m_llThreshold;
    uint64_t m_ll128Threshold;
    uint64_t m_localComputeDelayNs;     // Local reduce compute delay (ns)
    uint64_t m_interNodeComputeDelayNs; // Inter-node reduce compute delay (ns)
    uint64_t m_localComputePerByteNs;     // Per-byte compute rate for local reduce (ns/B)
    uint64_t m_interNodeComputePerByteNs; // Per-byte compute rate for inter-node reduce (ns/B)
    uint64_t m_localComputeBaseLLNs;      // Protocol-aware base compute for local reduce LL
    uint64_t m_localComputeBaseLL128Ns;   // Protocol-aware base compute for local reduce LL128
    uint64_t m_localComputeBaseSIMPLENs;  // Protocol-aware base compute for local reduce SIMPLE
    uint64_t m_interNodeComputeBaseLLNs;      // Protocol-aware base compute for inter reduce LL
    uint64_t m_interNodeComputeBaseLL128Ns;   // Protocol-aware base compute for inter reduce LL128
    uint64_t m_interNodeComputeBaseSIMPLENs;  // Protocol-aware base compute for inter reduce SIMPLE
    uint64_t m_interNodeBwRampDelayNs;  // Max ramp-up delay for inter-node phases (ns)
    uint64_t m_interNodeBwRampThreshold; // Chunk size threshold for ramp-up decay (bytes)
    uint64_t m_fullmeshPerStepLLNs;       // Per-step delay for LL protocol in fullmesh (ns)
    uint64_t m_fullmeshPerStepLL128Ns;   // Per-step delay for LL128 protocol in fullmesh (ns)
    uint64_t m_fullmeshPerStepSIMPLENs;  // Per-step delay for SIMPLE protocol in fullmesh (ns)

    // Per-GPU tracking for current phase
    std::vector<uint64_t> m_gpuReceivedBytes;
    std::vector<bool> m_gpuPhaseComplete;
    uint32_t m_completedGpuCount;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
    std::vector<uint8_t> m_dummyData;
};

} // namespace ns3

#endif // HIERARCHICAL_REDUCESCATTER_H