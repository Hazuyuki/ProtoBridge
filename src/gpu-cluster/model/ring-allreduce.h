/*
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
    uint16_t GetSendTarget(uint16_t gpu, uint32_t step);
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

    std::vector<uint32_t> m_gpuCompletedStep;
    std::vector<uint64_t> m_gpuStepReceivedBytes;
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
