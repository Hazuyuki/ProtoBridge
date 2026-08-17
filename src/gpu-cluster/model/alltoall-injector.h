/*
 * SPDX-License-Identifier: GPL-2.0-only
 * alltoall-injector.h
 *
 * AlltoAll collective injector.
 * - Ring topology: pipelined N-1 steps, each sends chunk to next ring neighbor.
 * - Switched topology: all N-1 chunks sent simultaneously to all destinations.
 * After all chunks delivered, every GPU has exchanged data with all others.
 */

#ifndef ALLTOALL_INJECTOR_H
#define ALLTOALL_INJECTOR_H

#include "collective-injector.h"
#include "fabric-endpoint.h"

#include <map>
#include <vector>
#include <functional>

namespace ns3
{

enum class AlltoAllState
{
    IDLE,
    RUNNING,
    COMPLETED
};

enum class AlltoAllMode
{
    RING_PIPELINE,      // Sequential ring steps (ring topology)
    CONCURRENT,         // All chunks sent simultaneously (switched topology)
    TWO_DIMENSIONAL     // Concurrent sends over one or two row/column links
};

class AlltoAllInjector : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    AlltoAllInjector();
    virtual ~AlltoAllInjector();

    // CollectiveInjector overrides
    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    AlltoAllState GetState() const { return m_state; }
    void SetSegmentSize(uint64_t bytes);
    void SetConcurrentMode(bool concurrent);
    void SetTwoDimensionalRouting(uint32_t rows, uint32_t cols);

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerInitialSends();
    void TriggerConcurrentSends();
    void SendGpuStepData(uint16_t gpu, uint32_t step);
    void SendGpuConcurrentChunk(uint16_t gpu, uint16_t dest);
    void ForwardTwoDimensionalChunk(uint16_t intermediate,
                                    uint16_t finalDest,
                                    uint16_t flowId);
    void OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step);
    void CheckGlobalCompletion();
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_chunkSize;  // dataSize / numGpus per destination
    uint8_t m_protocolId;

    AlltoAllState m_state;
    AlltoAllMode m_mode;
    uint64_t m_startupDelayNs;

    // Ring pipeline mode: per-GPU step tracking
    std::vector<uint32_t> m_gpuCompletedStep;
    std::vector<uint64_t> m_gpuStepReceivedBytes;
    uint32_t m_totalSteps;

    // Concurrent mode: per-GPU received bytes tracking (from all sources)
    std::vector<uint64_t> m_gpuReceivedBytes;
    std::vector<uint64_t> m_gpuSourcesReceived;  // count of completed source GPUs

    uint32_t m_rows;
    uint32_t m_cols;
    std::map<std::pair<uint16_t, uint16_t>, uint64_t> m_firstHopReceivedBytes;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;
    uint32_t m_totalCompletedGpus;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // ALLTOALL_INJECTOR_H
