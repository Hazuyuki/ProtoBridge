/*
 * SPDX-License-Identifier: GPL-2.0-only
 * fullmesh-allreduce.h
 *
 * Full-Mesh AllReduce: utilizes all N-1 links simultaneously.
 * Each GPU sends dataSize/N to each of N-1 other GPUs in parallel.
 *
 * Algorithm:
 *   Reduce-Scatter: each GPU splits data into N chunks, sends each
 *     chunk to a different GPU via a different link. All N-1 sends
 *     happen simultaneously. Total per-link data = dataSize/N.
 *   AllGather: same pattern in reverse. Each GPU sends its reduced
 *     chunk to all N-1 others simultaneously.
 *
 * Total time ≈ startupDelay + 2 * dataSize/N / perLinkBW
 * Effective throughput = N * perLinkBW (using all links + bidirectional)
 */

#ifndef FULLMESH_ALLREDUCE_H
#define FULLMESH_ALLREDUCE_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <vector>
#include <functional>

namespace ns3
{

enum class FullMeshArState
{
    IDLE,
    REDUCE_SCATTER,
    ALL_GATHER,
    COMPLETED
};

class FullMeshAllReduce : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    FullMeshAllReduce();
    virtual ~FullMeshAllReduce();

    // CollectiveInjector overrides
    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    FullMeshArState GetState() const { return m_state; }

    /// Test probe: chunk `gpu`'s size in the coverage grid.
    uint64_t ProbeChunkSizeOf(uint16_t gpu) const
    {
        return ChunkSizeOf(gpu);
    }
    /// Cumulative effective bytes the wire path sent (coverage probe).
    uint64_t GetSentDataBytes() const { return m_sentDataBytes; }
    /// Test probe: per-phase completion threshold for destination `dest`.
    uint64_t ProbeExpectedBytesPerDest(uint16_t dest) const
    {
        return ExpectedBytesPerDest(dest);
    }
    /// Test probe: reduce-scatter threshold for destination `dest`:
    /// (N-1) copies of its own chunk.
    uint64_t ProbeRsExpectedBytes(uint16_t dest) const
    {
        return static_cast<uint64_t>(m_numGpus - 1) * ChunkSizeOf(dest);
    }
    /// Test probe: effective bytes destination `dest` received during the
    /// reduce-scatter phase (per-rank load pin).
    uint64_t ProbeRsReceivedBytes(uint16_t dest) const
    {
        return (dest < m_rsReceivedBytes.size()) ? m_rsReceivedBytes[dest] : 0;
    }

private:
    void DoDispose() override;
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerReduceScatterPhase();
    void TriggerAllGatherPhase();
    void CheckReduceScatterComplete();
    void CheckAllGatherComplete();
    void Complete();
    // Chunk `gpu` of the grid: base dataSize/numGpus bytes plus one byte
    // for the first dataSize % numGpus chunks, so the grid sums to exactly
    // dataSize.
    uint64_t ChunkSizeOf(uint16_t gpu) const;
    // Bytes one destination receives per phase: the whole grid minus its
    // own chunk.
    uint64_t ExpectedBytesPerDest(uint16_t dest) const;

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint64_t m_chunkSize;  // dataSize / numGpus (per-destination amount)
    /// Number of one-byte-larger chunks in the grid (dataSize % numGpus).
    uint64_t m_chunkRemainder;
    uint16_t m_flowId;
    uint8_t m_protocolId;

    FullMeshArState m_state;
    uint64_t m_startupDelayNs;

    // Per-GPU tracking: bytes received in current phase
    std::vector<uint64_t> m_gpuReceivedBytes;
    // Per-GPU tracking: number of source GPUs that have fully sent to this GPU
    std::vector<uint32_t> m_gpuSrcDoneCount;

    uint32_t m_reduceScatterDoneCount;
    uint32_t m_allGatherDoneCount;

    /// Cumulative effective bytes the wire path sent (coverage probe).
    uint64_t m_sentDataBytes;
    // Per-GPU tracking: bytes received during the reduce-scatter phase
    // (persists across the phase switch so tests can pin per-rank load).
    std::vector<uint64_t> m_rsReceivedBytes;

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // FULLMESH_ALLREDUCE_H