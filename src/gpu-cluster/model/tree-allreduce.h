/*
 * SPDX-License-Identifier: GPL-2.0-only
 * tree-allreduce.h
 *
 * Chunk-pipelined Binary Tree AllReduce: bottom-up reduce + top-down
 * broadcast, with the FULL data size D carried over every tree edge in each
 * direction (D up during reduce, D down during broadcast), forwarded in
 * pipeline chunks so the hops overlap.
 *
 * Volume model (matches real NCCL tree AllReduce):
 * - Reduce: every non-root GPU sends D bytes to its parent; a parent forwards
 *   byte ranges up only once received from ALL children (min across children).
 * - Broadcast: the root re-broadcasts reduced byte ranges down; interior GPUs
 *   forward what they received from their parent to their children.
 * - Steady-state runtime is bounded below by D / edgeBW plus pipeline fill;
 *   the pre-2026-09 model moved only D/N per edge per direction and could
 *   beat that physical bound (see doc/CALIBRATION.md, Demystifying NCCL).
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

    // Reduce bytes available for forwarding up (or, at the root, for
    // re-broadcasting down): min over children of bytes received, clamped to
    // m_dataSize; leaves have all D locally.
    uint64_t UpReadyBytes(uint16_t gpu) const;
    void PumpUp(uint16_t gpu);
    void PumpDown(uint16_t gpu);
    // Per-hop software pacing: perStep once at the flow's first chunk,
    // perByte as a rate tax; returns the delay from now to the send instant.
    uint64_t PacedSendDelayNs(uint64_t& nextFreeNs, bool firstOfFlow, uint64_t bytes);
    void DoSend(uint16_t gpu, uint16_t target, uint64_t bytes, uint16_t flowId);
    void ScheduleOrSend(uint16_t gpu, uint16_t target, uint64_t bytes,
                        uint16_t flowId, uint64_t delayNs);
    void MarkGpuDone(uint16_t gpu);
    void CheckGlobalCompletion();
    void Complete();

    uint16_t GetParent(uint16_t gpu) const;
    uint16_t GetLeftChild(uint16_t gpu) const;
    uint16_t GetRightChild(uint16_t gpu) const;
    uint16_t GetTreeDepth() const;
    uint16_t GetTreeLevel(uint16_t gpu) const;

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_chunkSize;      // D/N, kept for protocol-threshold selection
    uint64_t m_pipeChunkSize;  // pipeline forwarding granularity
    uint16_t m_flowId;
    uint8_t m_protocolId;
    uint16_t m_depth;
    uint32_t m_totalSteps;  // 2 * depth (kept for the progress accessors)

    TreeAllReduceState m_state;
    uint64_t m_startupDelayNs;
    uint64_t m_perStepSwOverheadNs;
    uint64_t m_swOverheadPerByteNs;

    // Byte-counting flow state (order-insensitive: spray reorder and GBN
    // duplicates cannot regress progress; overshoot past D is clamped).
    std::vector<uint64_t> m_upRecvFromChild;  // [child rank] bytes its parent received
    std::vector<uint64_t> m_upSent;           // bytes forwarded to parent
    std::vector<uint64_t> m_downRecv;         // bytes received from parent
    std::vector<uint64_t> m_downSent;         // bytes forwarded to each child
    std::vector<uint64_t> m_upNextFreeNs;     // sw-pacing state, reduce flow
    std::vector<uint64_t> m_downNextFreeNs;   // sw-pacing state, broadcast flow
    std::vector<bool> m_upDone;
    std::vector<bool> m_downDone;

    // Coarse per-GPU progress for the scratch diagnostics histogram.
    std::vector<uint32_t> m_gpuCurrentStep;
    uint32_t m_gpuDoneCount;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // TREE_ALLREDUCE_H
