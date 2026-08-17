/*
 * SPDX-License-Identifier: GPL-2.0-only
 * tree-allreduce.h
 *
 * Pipelined Binary Tree AllReduce: bottom-up reduce + top-down broadcast,
 * with per-chunk pipeline overlap.
 *
 * For N=8 GPUs with depth=3:
 * - Each chunk goes through 6 steps (3 reduce + 3 broadcast)
 * - Reduce step k: GPUs at level (depth-k) send chunkSize to parent
 * - Broadcast step k: GPUs at level k send chunkSize to children
 * - Multiple chunks pipeline: chunk 0 broadcast overlaps chunk 1 reduce
 */

#ifndef TREE_ALLREDUCE_H
#define TREE_ALLREDUCE_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class TreeAllReduceState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class TreeAllReduce : public CollectiveInjector
{
  public:
    static TypeId GetTypeId();

    TreeAllReduce();
    virtual ~TreeAllReduce();

    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    void SetPerStepSwOverhead(Time delay);
    void SetSwOverheadPerByteNs(uint64_t nsPerByte);
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    TreeAllReduceState GetState() const { return m_state; }
    uint32_t GetCompletedGpuCount() const { return m_gpuDoneCount; }
    uint32_t GetTotalSteps() const { return m_totalSteps; }
    const std::vector<uint32_t>& GetGpuCurrentSteps() const { return m_gpuCurrentStep; }

  private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerInitialSends();
    void SendGpuStepData(uint16_t gpu, uint32_t step);
    void OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step);
    void AdvanceGpuStep(uint16_t gpu);
    void CheckGlobalCompletion();
    void Complete();

    uint16_t GetParent(uint16_t gpu) const;
    uint16_t GetLeftChild(uint16_t gpu) const;
    uint16_t GetRightChild(uint16_t gpu) const;
    uint16_t GetTreeDepth() const;
    uint16_t GetTreeLevel(uint16_t gpu) const;
    bool IsReduceStep(uint32_t step) const;
    bool IsBroadcastStep(uint32_t step) const;
    uint32_t GetStepFromFlowId(uint16_t flowId) const;

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_chunkSize;
    uint16_t m_flowId;
    uint8_t m_protocolId;
    uint16_t m_depth;
    uint32_t m_totalSteps;  // 2 * depth

    TreeAllReduceState m_state;
    uint64_t m_startupDelayNs;
    uint64_t m_perStepSwOverheadNs;
    uint64_t m_swOverheadPerByteNs;

    // Per-GPU pipeline tracking
    std::vector<uint32_t> m_gpuCurrentStep;
    std::vector<std::vector<uint64_t>> m_gpuStepReceivedBytes;
    uint32_t m_gpuDoneCount;

    // Pre-computed per-step targets
    // m_stepSendTarget[step][gpu] = dest rank to send to (0xFFFF = no send)
    std::vector<std::vector<uint16_t>> m_stepSendTarget;
    // m_stepReceiveThreshold[step][gpu] = bytes to receive (0 = no receive)
    std::vector<std::vector<uint64_t>> m_stepReceiveThreshold;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // TREE_ALLREDUCE_H
