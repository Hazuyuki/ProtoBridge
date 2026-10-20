/*
 * SPDX-License-Identifier: GPL-2.0-only
 * nccl-ring-allreduce.h
 *
 * Real-NCCL-style global pipelined ring AllReduce for multi-node clusters.
 *
 * Unlike HierarchicalAllReduce (4 barrier-separated phases), this models
 * what NCCL's Ring algorithm actually does at multi-node scale:
 *  - C channels (rings), each spanning ALL N ranks. Channel c enters node n
 *    at local rank c, walks all gpusPerNode GPUs over the Su fabric, and its
 *    exit GPU (local (c+g-1)%g) crosses to node n+1's entry GPU (local c)
 *    over that GPU's NIC — one NIC in / one NIC out per ring per node, with
 *    the C channels rotated so all rails carry a crossing (rail-aligned).
 *  - Chunk-level pipelining: each channel carries dataSize/C, split into N
 *    per-step chunks of dataSize/(C*N); 2(N-1) same-direction steps
 *    (reduce-scatter then all-gather) advance per-rank as soon as the
 *    previous step's chunk arrives — no global barrier, so intra-node and
 *    inter-node hops overlap and completion tracks the slowest hop instead
 *    of the sum of phases.
 */

#ifndef NCCL_RING_ALLREDUCE_H
#define NCCL_RING_ALLREDUCE_H

#include "collective-injector.h"
#include "fabric-endpoint.h"
#include "fabric-header.h"

#include <functional>
#include <vector>

namespace ns3
{

class NcclRingAllReduce : public CollectiveInjector
{
public:
    static TypeId GetTypeId();

    NcclRingAllReduce();
    virtual ~NcclRingAllReduce();

    // CollectiveInjector overrides
    void Initialize(uint16_t numGpus, uint64_t dataSize,
                    const std::vector<Ptr<FabricEndpoint>>& endpoints) override;
    void SetCompletionCallback(std::function<void(uint64_t durationNs)> cb) override;
    void Start() override;
    void SetStartupDelay(Time delay) override;
    CollectiveType GetCollectiveType() const override;
    bool IsCompleted() const override;

    // Multi-node ring construction (node-major rank layout: rank = node*g+local)
    void SetGpusPerNode(uint32_t gpusPerNode);
    void SetNumNodes(uint32_t numNodes);
    // Number of rings; 0 = auto (gpusPerNode, one crossing per rail)
    void SetNumChannels(uint32_t numChannels);

    // Per-step reduce/software overhead (same semantics as RingAllReduce):
    // swOverhead = baseDelay(chunkSize) + perByteNs * chunkSize
    void SetPerStepSwOverhead(Time delay);
    void SetSwOverheadPerByteNs(uint64_t perByteNs);
    void SetComputeBaseDelays(Time ll, Time ll128, Time simple);
    uint64_t GetPerStepSwOverheadNs(uint64_t chunkSize) const;
    void SetLlThreshold(uint64_t threshold);
    void SetLl128Threshold(uint64_t threshold);

    /**
     * @brief Effective data bytes the ring has put on the wire so far.
     *
     * Coverage probe for tests: the chunk grid must carry the FULL data
     * size (total traffic 2(N-1)*D), not the truncated floor.
     */
    uint64_t GetSentDataBytes() const { return m_sentDataBytes; }

private:
    void DoDispose() override;
    void BuildRings();
    void SetupReceiveCallbacks();
    void OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header);
    void TriggerInitialSends();
    void SendStepData(uint16_t gpu, uint32_t channel, uint32_t step);
    void OnStepReceiveComplete(uint16_t gpu, uint32_t channel, uint32_t step);
    void Complete();

    uint16_t m_numGpus;
    uint64_t m_dataSize;
    uint32_t m_gpusPerNode;
    uint32_t m_numNodes;
    uint32_t m_numChannels;     // resolved channel count (>=1)
    uint32_t m_numChannelsCfg;  // configured value (0 = auto)
    uint64_t m_chunkSize;       // dataSize / (channels * numGpus), >= 1
    uint64_t m_chunkRemainder;  // dataSize % (channels * numGpus) — first chunks carry one extra byte
    uint32_t m_totalSteps;      // 2 * (numGpus - 1)

    /**
     * @brief Size of the chunk rank `rank` sends on `channel` at `step`.
     *
     * The tensor splits into channels*numGpus chunks in (channel, index)
     * row-major order; the first m_chunkRemainder chunks carry one extra
     * byte so the grid sums to exactly dataSize (no truncation). Rank r's
     * step-s send moves chunk (r - s) mod numGpus of the channel.
     */
    uint64_t ChunkSizeOf(uint32_t channel, uint16_t rank, uint32_t step) const;

    /**
     * @brief Bytes destRank expects to receive on `channel` for `step`.
     *
     * The ring predecessor's ChunkSizeOf for the same step — the receive
     * ledger advances one step per expected chunk.
     */
    uint64_t ExpectedRecvBytes(uint16_t destRank, uint32_t channel,
                               uint32_t step) const;
    uint16_t m_flowIdBase;
    uint8_t m_protocolId;

    bool m_running;
    bool m_completed;
    uint64_t m_startupDelayNs;
    uint64_t m_perStepSwOverheadNs;
    uint64_t m_swOverheadPerByteNs;
    uint64_t m_computeBaseLLNs;
    uint64_t m_computeBaseLL128Ns;
    uint64_t m_computeBaseSIMPLENs;
    uint64_t m_llThreshold;
    uint64_t m_ll128Threshold;

    // Ring successor per channel: m_ringNext[channel][rank]
    std::vector<std::vector<uint16_t>> m_ringNext;
    // Per (rank, channel) pipeline state, indexed rank * m_numChannels + channel
    std::vector<uint32_t> m_completedStep;
    std::vector<uint64_t> m_recvBytes;
    std::vector<uint32_t> m_doneChannels; // per rank
    uint32_t m_gpuDoneCount;
    uint32_t m_permanentLossCount;
    uint64_t m_sentDataBytes;      ///< cumulative effective bytes sent (coverage probe)

    uint64_t m_startTimeNs;
    uint64_t m_endTimeNs;

    std::vector<Ptr<FabricEndpoint>> m_endpoints;
    std::function<void(uint64_t durationNs)> m_completionCallback;
};

} // namespace ns3

#endif // NCCL_RING_ALLREDUCE_H
