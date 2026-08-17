/*
 * SPDX-License-Identifier: GPL-2.0-only
 * hierarchical-allgather.h
 *
 * Hierarchical AllGather for multi-node GPU clusters.
 * 3-phase algorithm:
 * 1. Local all-gather (NVLink/MetaXLink within node)
 * 2. Inter-node all-gather (RDMA across nodes)
 * 3. Complete
 */

#ifndef HIERARCHICAL_ALLGATHER_H
#define HIERARCHICAL_ALLGATHER_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>
#include <unordered_map>

namespace ns3
{

enum class HierAllGatherPhase
{
    LOCAL_ALLGATHER,
    INTER_ALLGATHER,
    COMPLETED
};

enum class HierAllGatherState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class HierarchicalAllGather : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    HierarchicalAllGather();
    virtual ~HierarchicalAllGather();

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
    void SetStartupDelays(Time ll, Time ll128, Time simple);
    void SetStartupPerGpuNs(uint64_t perGpuNs);
    void SetLlThreshold(uint64_t threshold);
    void SetLl128Threshold(uint64_t threshold);

    // Protocol-aware inter-node startup delays: separate LL/LL128/SIMPLE values
    // for inter-node phase transitions. Unlike global GetPhaseStartupDelayNs()
    // which adds perGpu*numGpus barrier, inter-node startup only needs protocol
    // delay since GPUs are already synchronized after local phases.
    void SetInterNodeStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetInterNodeStartupDelayNs(uint64_t chunkSize) const;

    // Protocol-aware local phase startup delays: separate LL/LL128/SIMPLE values
    // for local phase (Phase 1 start). Models persistent kernel wakeup — near-zero for LL.
    void SetLocalStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetLocalStartupDelayNs(uint64_t chunkSize) const;

    // Inter-node BW ramp-up delay (models RDMA pipeline ramp-up)
    void SetInterNodeBwRamp(Time rampDelay, uint64_t rampThresholdBytes);
    uint64_t GetInterNodeRampDelayNs(uint64_t chunkSize) const;

    // Fullmesh per-step delay: models step-by-step pipeline overhead within
    // fullmesh local phases. Uses the same LL/LL128/SIMPLE protocol thresholds
    // as GetPhaseStartupDelayNs. Total per phase = (gpusPerNode-1) * per-step delay.
    void SetFullmeshPerStepStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetFullmeshStepDelayNs(uint64_t chunkSize) const;

    HierAllGatherState GetState() const { return m_state; }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);

    // Phase management
    void StartLocalAllGather();
    void OnLocalAllGatherChunkReceived(uint16_t destRank, uint32_t size);
    void CheckLocalAllGatherComplete(uint16_t gpu);
    void StartInterNodeAllGather();
    void OnInterNodeAllGatherChunkReceived(uint16_t destRank, uint32_t size);
    void CheckInterNodeAllGatherComplete(uint16_t gpu);
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
    uint64_t m_chunkSize;       // dataSize / totalGpus (per-GPU chunk)
    uint64_t m_nodeChunkSize;   // chunkSize * gpusPerNode (per-node combined chunk)

    HierAllGatherState m_state;
    HierAllGatherPhase m_currentPhase;
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
    uint64_t m_interNodeBwRampDelayNs;
    uint64_t m_interNodeBwRampThreshold;
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

#endif // HIERARCHICAL_ALLGATHER_H