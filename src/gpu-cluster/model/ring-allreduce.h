/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-allreduce.h
 *
 * Pipelined Ring AllReduce Coordinator. Derives from CollectiveInjector.
 * Each GPU independently advances through steps upon receiving its
 * segment, without waiting for a global barrier between steps.
 */

#ifndef RING_ALLREDUCE_H
#define RING_ALLREDUCE_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"
#include "protocol-transaction.h"

#include <functional>
#include <vector>

namespace ns3
{

enum class RingState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class RingAllReduce : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    RingAllReduce();
    virtual ~RingAllReduce();

    // CollectiveInjector overrides
    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    void SetPerStepSwOverhead(Time delay);

    // Protocol-aware + data-proportional per-step software overhead delays:
    // swOverhead = baseDelay(chunkSize) + perByteNs * chunkSize
    void SetSwOverheadPerByteNs(uint64_t perByteNs);
    void SetComputeBaseDelays(Time ll, Time ll128, Time simple);
    uint64_t GetPerStepSwOverheadNs(uint64_t chunkSize) const;
    void SetLlThreshold(uint64_t threshold);
    void SetLl128Threshold(uint64_t threshold);
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    RingState GetState() const { return m_state; }

    /// Cumulative effective bytes the wire path sent (coverage probe).
    uint64_t GetSentDataBytes() const { return m_sentDataBytes; }

    /// Test probe: SegmentSizeOf (public wrapper — the chunk-identity
    /// invariant tests read it without simulating a full collective).
    uint64_t ProbeSegmentSizeOf(uint16_t gpu, uint32_t step) const
    {
        return SegmentSizeOf(gpu, step);
    }

    /// Test probe: GetSendTarget (public wrapper for the same tests).
    uint16_t ProbeSendTarget(uint16_t gpu, uint32_t step) const
    {
        return GetSendTarget(gpu, step);
    }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void BuildTransactionGraph();
    void ExecuteTransactionAction(const ProtocolTransactionAction& action);
    void OnTransactionNodeComplete(ProtocolTransactionNodeId nodeId);
    void OnTransactionComplete();
    void TriggerInitialSends();
    void SendGpuStepData(uint16_t gpu, uint32_t step);
    void OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step);
    void CheckGlobalCompletion();
    uint16_t GetSendTarget(uint16_t gpu, uint32_t step) const;
    // Segment size (in bytes) that `gpu` transmits at `step`: ownership of
    // segment (g - s) mod N in Reduce-Scatter and (g + s - 2(N-1)) mod N in
    // AllGather (the direction reversal), with the first
    // dataSize % numGpus segments one byte larger so the grid sums to
    // exactly dataSize.
    uint64_t SegmentSizeOf(uint16_t gpu, uint32_t step) const;
    /// Bytes destRank must receive at `step` (the sender's segment size —
    /// the GPU whose step-`step` send targets destRank).
    uint64_t StepExpectedBytes(uint16_t destRank, uint32_t step) const;
    bool IsReduceScatterStep(uint32_t step) const;
    bool IsAllGatherStep(uint32_t step) const;
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_segmentSize;
    uint16_t m_flowId;
    uint8_t m_protocolId;
    bool m_useTransactionModel;

    RingState m_state;
    uint64_t m_startupDelayNs;
    uint64_t m_perStepSwOverheadNs;
    uint64_t m_swOverheadPerByteNs;
    uint64_t m_computeBaseLLNs;
    uint64_t m_computeBaseLL128Ns;
    uint64_t m_computeBaseSIMPLENs;
    uint64_t m_llThreshold;
    uint64_t m_ll128Threshold;
    uint64_t m_chunkSize;
    /// Number of one-byte-larger segments in the grid (dataSize % numGpus).
    uint64_t m_segmentRemainder;
    /// Cumulative effective bytes the wire path sent (coverage probe; the
    /// transaction path derives its byte weights from SegmentSizeOf instead).
    uint64_t m_sentDataBytes;

    std::vector<uint32_t> m_gpuCompletedStep;
    /// Wire-path receive ledger, keyed (dest rank, SENDER step) — the step
    /// is decoded from the step-encoded flowId, not taken from this
    /// receiver's own step counter. A single per-dest counter mixed bytes
    /// of different steps whenever the pipelined senders drifted ahead of
    /// the receiver, or a retransmission delivered a late copy after the
    /// counter had already advanced (codex round 14, finding 3).
    std::vector<std::vector<uint64_t>> m_gpuStepReceivedBytes;
    uint32_t m_gpuDoneCount;
    uint32_t m_permanentLossCount;
    uint32_t m_reduceScatterSteps;
    uint32_t m_allGatherSteps;
    uint32_t m_totalSteps;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    ProtocolTransactionGraph m_transactionGraph;
    Ptr<ProtocolTransactionExecutor> m_transactionExecutor;
    std::vector<uint16_t> m_transactionGpuByNode;
    std::vector<uint32_t> m_transactionStepByNode;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // RING_ALLREDUCE_H
