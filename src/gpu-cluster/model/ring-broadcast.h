/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-broadcast.h
 *
 * Chunk-pipelined Ring Broadcast injector. Derives from CollectiveInjector.
 * The root streams the FULL dataSize D around the ring in pipeline chunks;
 * every non-root GPU receives D on its single inbound ring edge and forwards
 * it to its ring successor (the GPU whose successor is the root does not
 * forward).  Per-edge volume is D, so the completion time respects the
 * physical lower bound D/BW (plus pipeline fill).
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
    uint16_t RingNextOf(uint16_t gpu) const;
    void PumpForward(uint16_t gpu);
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_chunkSize;      // D/N, protocol-threshold selection only
    uint64_t m_pipeChunkSize;  // pipeline forwarding quantum
    uint16_t m_flowId;
    uint16_t m_rootRank;
    uint8_t m_protocolId;

    BroadcastState m_state;
    uint64_t m_startupDelayNs;

    // Byte-counted per-GPU progress (order-insensitive)
    std::vector<uint64_t> m_recvBytes;  // received from ring predecessor
    std::vector<uint64_t> m_fwdBytes;   // forwarded to ring successor
    uint32_t m_gpuDoneCount;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // RING_BROADCAST_H
