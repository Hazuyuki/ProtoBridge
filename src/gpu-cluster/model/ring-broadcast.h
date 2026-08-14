/*
 * ring-broadcast.h
 *
 * Ring Broadcast injector. Derives from CollectiveInjector.
 * Root rank has dataSize data. N-1 steps clockwise:
 * each step, current holder forwards segment to (gpu+1)%N.
 * After N-1 steps all GPUs have root's data.
 * Needs rootRank parameter.
 */

#ifndef RING_BROADCAST_H
#define RING_BROADCAST_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class BroadcastState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class RingBroadcast : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    RingBroadcast();
    virtual ~RingBroadcast();

    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    void SetRootRank(uint16_t rootRank);

    BroadcastState GetState() const { return m_state; }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerInitialSends();
    void SendStepData(uint16_t sender, uint32_t step);
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_segmentSize;
    uint16_t m_flowId;
    uint16_t m_rootRank;
    uint8_t m_protocolId;

    BroadcastState m_state;
    uint64_t m_startupDelayNs;

    // Sequential tracking: one sender at a time
    uint16_t m_currentSenderRank;
    uint32_t m_currentStep;
    uint64_t m_senderReceivedBytes;
    uint32_t m_gpuDoneCount;
    uint32_t m_totalSteps;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // RING_BROADCAST_H