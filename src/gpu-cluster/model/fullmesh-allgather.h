/*
 * SPDX-License-Identifier: GPL-2.0-only
 * fullmesh-allgather.h
 *
 * Full-Mesh AllGather: each GPU sends its chunk to all N-1 others simultaneously.
 * Total per-link data = dataSize / N.
 */

#ifndef FULLMESH_ALLGATHER_H
#define FULLMESH_ALLGATHER_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class FullMeshAgState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class FullMeshAllGather : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    FullMeshAllGather();
    virtual ~FullMeshAllGather();

    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    FullMeshAgState GetState() const { return m_state; }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerSends();
    void CheckCompletion();
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_chunkSize;
    uint16_t m_flowId;
    uint8_t m_protocolId;

    FullMeshAgState m_state;
    uint64_t m_startupDelayNs;

    std::vector<uint64_t> m_gpuReceivedBytes;
    std::vector<uint32_t> m_gpuSrcDoneCount;
    uint32_t m_doneCount;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // FULLMESH_ALLGATHER_H