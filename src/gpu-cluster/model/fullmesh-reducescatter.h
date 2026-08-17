/*
 * SPDX-License-Identifier: GPL-2.0-only
 * fullmesh-reducescatter.h
 *
 * Full-Mesh ReduceScatter: each GPU sends its chunk to all N-1 others simultaneously.
 * Same as AllGather but single phase (no gather back).
 */

#ifndef FULLMESH_REDUCESCATTER_H
#define FULLMESH_REDUCESCATTER_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class FullMeshRsState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class FullMeshReduceScatter : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    FullMeshReduceScatter();
    virtual ~FullMeshReduceScatter();

    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    FullMeshRsState GetState() const { return m_state; }

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

    FullMeshRsState m_state;
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

#endif // FULLMESH_REDUCESCATTER_H