/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-reduce.h
 *
 * Chunk-pipelined Ring Reduce injector. Derives from CollectiveInjector.
 * Data flows counterclockwise toward the root along a chain of N-1 ring
 * edges: the chain head (the root's ring predecessor) streams its FULL
 * dataSize D; every interior GPU forwards the running element-wise partial
 * sum (still D bytes — reduction does not shrink the array) after folding in
 * its local contribution.  Per-edge volume is D, so the completion time
 * respects the physical lower bound D/BW (plus pipeline fill).  The root is
 * done when it has received D from its ring successor.
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
    uint16_t RingPrevOf(uint16_t gpu) const;
    void PumpForward(uint16_t gpu);
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_chunkSize;      // D/N, protocol-threshold selection only
    uint64_t m_pipeChunkSize;  // pipeline forwarding quantum
    uint16_t m_flowId;
    uint16_t m_rootRank;
    uint16_t m_headRank;       // chain head = the root's ring predecessor
    uint8_t m_protocolId;

    ReduceState m_state;
    uint64_t m_startupDelayNs;

    // Byte-counted per-GPU progress (order-insensitive)
    std::vector<uint64_t> m_recvBytes;  // received from ring successor
    std::vector<uint64_t> m_fwdBytes;   // forwarded to ring predecessor

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // RING_REDUCE_H
