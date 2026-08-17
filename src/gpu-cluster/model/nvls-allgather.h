/*
 * SPDX-License-Identifier: GPL-2.0-only
 * nvls-allgather.h
 *
 * NVLS AllGather: switch-based concatenation through NVSwitch.
 *
 * Algorithm:
 * - Phase 1: each GPU sends its chunk (dataSize/N) to the switch
 * - Switch concatenates all N chunks into full dataSize
 * - Phase 2: switch multicasts the concatenated result to all GPUs
 *
 * Requires "switched" topology with NVSwitch that has AllGatherEnabled=true.
 */

#ifndef NVLS_ALLGATHER_H
#define NVLS_ALLGATHER_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class NvlsAllGatherState
{
    IDLE,
    SEND_PHASE,
    BROADCAST_PHASE,
    COMPLETED
};

class NvlsAllGather : public CollectiveInjector
{
  public:
    static TypeId GetTypeId();

    NvlsAllGather();
    virtual ~NvlsAllGather();

    // CollectiveInjector overrides
    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    NvlsAllGatherState GetState() const { return m_state; }

  private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerSendPhase();
    void OnGpuBroadcastReceiveComplete(uint16_t gpu);
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_chunkSize;  // dataSize / numGpus
    uint16_t m_flowId;

    NvlsAllGatherState m_state;
    uint64_t m_startupDelayNs;

    // Track received bytes per GPU for broadcast phase
    std::vector<uint64_t> m_gpuBroadcastReceivedBytes;
    uint32_t m_broadcastDoneCount;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // NVLS_ALLGATHER_H