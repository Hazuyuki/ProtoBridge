/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-allgather.h
 *
 * Pipelined Ring AllGather injector. Derives from CollectiveInjector.
 * Each GPU starts with dataSize/N data. N-1 steps counterclockwise:
 * each step, GPU sends its segment to (gpu-1+N)%N.
 * After N-1 steps every GPU has all N segments (full dataSize).
 */

#ifndef RING_ALLGATHER_H
#define RING_ALLGATHER_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class AllGatherState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class RingAllGather : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    RingAllGather();
    virtual ~RingAllGather();

    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    void SetPerStepSwOverhead(Time delay);
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    AllGatherState GetState() const { return m_state; }
    const std::vector<uint32_t>& GetGpuCompletedSteps() const
    {
        return m_gpuCompletedStep;
    }
    const std::vector<uint64_t>& GetGpuStepReceivedBytes() const
    {
        return m_gpuStepReceivedBytes;
    }

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
    uint8_t m_protocolId;

    AllGatherState m_state;
    uint64_t m_startupDelayNs;
    uint64_t m_perStepSwOverheadNs;

    std::vector<uint32_t> m_gpuCompletedStep;
    std::vector<uint64_t> m_gpuStepReceivedBytes;
    std::vector<std::vector<uint64_t>> m_gpuReceivedBytesByStep;
    uint32_t m_gpuDoneCount;
    uint32_t m_totalSteps;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // RING_ALLGATHER_H
