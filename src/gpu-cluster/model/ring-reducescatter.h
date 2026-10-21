/*
 * SPDX-License-Identifier: GPL-2.0-only
 * ring-reducescatter.h
 *
 * Pipelined Ring ReduceScatter injector. Derives from CollectiveInjector.
 * Each GPU starts with dataSize data. N-1 steps clockwise:
 * each step, GPU sends its segment to (gpu+1)%N.
 * After N-1 steps each GPU holds one reduced segment of dataSize/N.
 */

#ifndef RING_REDUCESCATTER_H
#define RING_REDUCESCATTER_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class ReduceScatterState
{
    IDLE,
    RUNNING,
    COMPLETED
};

class RingReduceScatter : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    RingReduceScatter();
    virtual ~RingReduceScatter();

    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    void SetPerStepSwOverhead(Time delay);
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;
    const std::vector<uint32_t>& GetGpuCompletedSteps() const
    {
        return m_gpuCompletedStep;
    }
    const std::vector<uint64_t>& GetGpuStepReceivedBytes() const
    {
        return m_gpuStepReceivedBytes;
    }

    ReduceScatterState GetState() const { return m_state; }

    /// Test probes for the chunk-identity / coverage invariants.
    uint64_t ProbeSegmentSizeOf(uint16_t gpu, uint32_t step) const
    {
        return SegmentSizeOf(gpu, step);
    }
    /// Cumulative effective bytes the wire path sent (coverage probe).
    uint64_t GetSentDataBytes() const { return m_sentDataBytes; }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    /// Fill m_ringPos from the endpoints' embedded ring neighbors (walk
    /// the clockwise direction from rank 0), with the identity fallback.
    void ComputeRingPositions();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerInitialSends();
    void SendGpuStepData(uint16_t gpu, uint32_t step);
    void OnGpuStepReceiveComplete(uint16_t gpu, uint32_t step);
    void CheckGlobalCompletion();
    void Complete();
    // Segment size (in bytes) that `gpu` transmits at `step`: ownership of
    // segment (ringPos - step) mod N (sends run clockwise), with the first
    // dataSize % numGpus segments one byte larger so the grid sums to
    // exactly dataSize.
    uint64_t SegmentSizeOf(uint16_t gpu, uint32_t step) const;
    /// Bytes destRank must receive at `step` (the sender's segment size —
    /// the GPU whose step-`step` send targets destRank).
    uint64_t StepExpectedBytes(uint16_t destRank, uint32_t step) const;

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_segmentSize;
    /// Number of one-byte-larger segments in the grid (dataSize % numGpus).
    uint64_t m_segmentRemainder;
    /// Ring POSITION of each rank (see ComputeRingPositions).
    std::vector<uint16_t> m_ringPos;
    uint16_t m_flowId;
    uint8_t m_protocolId;

    ReduceScatterState m_state;
    uint64_t m_startupDelayNs;
    uint64_t m_perStepSwOverheadNs;

    std::vector<uint32_t> m_gpuCompletedStep;
    std::vector<uint64_t> m_gpuStepReceivedBytes;
    std::vector<std::vector<uint64_t>> m_gpuReceivedBytesByStep;
    uint32_t m_gpuDoneCount;
    uint32_t m_totalSteps;

    /// Cumulative effective bytes the wire path sent (coverage probe).
    uint64_t m_sentDataBytes;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // RING_REDUCESCATTER_H
