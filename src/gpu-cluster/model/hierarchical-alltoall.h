/*
 * hierarchical-alltoall.h
 *
 * Hierarchical AlltoAll for multi-node GPU clusters.
 * 2-phase algorithm:
 * 1. Local alltoall (NVLink/MetaXLink within each node)
 * 2. Inter-node alltoall (RDMA across nodes)
 */

#ifndef HIERARCHICAL_ALLTOALL_H
#define HIERARCHICAL_ALLTOALL_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class HierAlltoAllPhase
{
    LOCAL_ALLTOALL,
    INTER_ALLTOALL,
    COMPLETED
};

enum class HierAlltoAllState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class HierarchicalAlltoAll : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    HierarchicalAlltoAll();
    virtual ~HierarchicalAlltoAll();

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

    // Protocol-aware inter-node startup delays
    void SetInterNodeStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetInterNodeStartupDelayNs(uint64_t chunkSize) const;

    // Protocol-aware local phase startup delays
    void SetLocalStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetLocalStartupDelayNs(uint64_t chunkSize) const;

    // Fullmesh per-step delay
    void SetFullmeshPerStepStartupDelays(Time ll, Time ll128, Time simple);
    uint64_t GetFullmeshStepDelayNs(uint64_t chunkSize) const;

    HierAlltoAllState GetState() const { return m_state; }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);

    // Phase management
    void StartLocalAlltoAll();
    void OnLocalAlltoAllChunkReceived(uint16_t destRank, uint32_t size);
    void CheckLocalAlltoAllComplete(uint16_t gpu);
    void StartInterNodeAlltoAll();
    void OnInterNodeAlltoAllChunkReceived(uint16_t destRank, uint32_t size);
    void CheckInterNodeAlltoAllComplete(uint16_t gpu);
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

    // Data sizes
    uint64_t m_chunkSize;       // dataSize / numGpus (per-GPU chunk sent to each dest)

    HierAlltoAllState m_state;
    HierAlltoAllPhase m_currentPhase;
    uint64_t m_startupDelayNs;
    uint64_t m_interNodeStartupDelayNs;
    uint64_t m_interNodeStartupLLNs;
    uint64_t m_interNodeStartupLL128Ns;
    uint64_t m_interNodeStartupSIMPLENs;
    uint64_t m_localStartupLLNs;
    uint64_t m_localStartupLL128Ns;
    uint64_t m_localStartupSIMPLENs;
    uint64_t m_startupLLNs;
    uint64_t m_startupLL128Ns;
    uint64_t m_startupSIMPLENs;
    uint64_t m_startupPerGpuNs;
    uint64_t m_llThreshold;
    uint64_t m_ll128Threshold;
    uint64_t m_fullmeshPerStepLLNs;
    uint64_t m_fullmeshPerStepLL128Ns;
    uint64_t m_fullmeshPerStepSIMPLENs;

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

#endif // HIERARCHICAL_ALLTOALL_H