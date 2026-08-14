/*
 * ring-reduce.h
 *
 * Pipelined Ring Reduce injector. Derives from CollectiveInjector.
 * Reverse of Broadcast: N-1 steps counterclockwise.
 * Each GPU sends its segment toward root.
 * After N-1 steps root has fully reduced dataSize.
 * Needs rootRank parameter.
 */

#ifndef RING_REDUCE_H
#define RING_REDUCE_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class ReduceState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class RingReduce : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    RingReduce();
    virtual ~RingReduce();

    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    void SetRootRank(uint16_t rootRank);

    ReduceState GetState() const { return m_state; }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerInitialSends();
    void SendGpuStepData(uint16_t gpu, uint32_t step);
    void OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step);
    void CheckGlobalCompletion();
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_segmentSize;
    uint16_t m_flowId;
    uint16_t m_rootRank;
    uint8_t m_protocolId;

    ReduceState m_state;
    uint64_t m_startupDelayNs;

    std::vector<uint32_t> m_gpuCompletedStep;
    std::vector<uint64_t> m_gpuStepReceivedBytes;
    uint32_t m_gpuDoneCount;
    uint32_t m_totalSteps;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // RING_REDUCE_H