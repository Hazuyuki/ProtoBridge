/*
 * SPDX-License-Identifier: GPL-2.0-only
 * nccl-ring-allreduce.cc
 *
 * Real-NCCL-style global pipelined ring AllReduce (see header).
 */

#include "nccl-ring-allreduce.h"

#include "nccl-protocol.h"
#include "protocol-model.h"

#include "ns3/log.h"
#include "ns3/simulator.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NcclRingAllReduce");

NS_OBJECT_ENSURE_REGISTERED(NcclRingAllReduce);

TypeId
NcclRingAllReduce::GetTypeId()
{
    static TypeId tid = TypeId("ns3::NcclRingAllReduce")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<NcclRingAllReduce>();
    return tid;
}

NcclRingAllReduce::NcclRingAllReduce()
    : m_numGpus(0),
      m_dataSize(0),
      m_gpusPerNode(8),
      m_numNodes(1),
      m_numChannels(1),
      m_numChannelsCfg(0),
      m_chunkSize(0),
      m_chunkRemainder(0),
      m_totalSteps(0),
      m_flowIdBase(1),
      m_protocolId(static_cast<uint8_t>(NcclProtocol::SIMPLE)),
      m_running(false),
      m_completed(false),
      m_trivialSingleRank(false),
      m_trivialZeroData(false),
      m_startupDelayNs(0),
      m_perStepSwOverheadNs(0),
      m_swOverheadPerByteNs(0),
      m_computeBaseLLNs(0),
      m_computeBaseLL128Ns(0),
      m_computeBaseSIMPLENs(0),
      m_llThreshold(8192),
      m_ll128Threshold(2 * 1024 * 1024),
      m_gpuDoneCount(0),
      m_permanentLossCount(0),
      m_sentDataBytes(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

NcclRingAllReduce::~NcclRingAllReduce()
{
}

void NcclRingAllReduce::SetGpusPerNode(uint32_t gpusPerNode) { m_gpusPerNode = gpusPerNode; }
void NcclRingAllReduce::SetNumNodes(uint32_t numNodes) { m_numNodes = numNodes; }
void NcclRingAllReduce::SetNumChannels(uint32_t numChannels) { m_numChannelsCfg = numChannels; }
void NcclRingAllReduce::SetPerStepSwOverhead(Time delay) { m_perStepSwOverheadNs = delay.GetNanoSeconds(); }
void NcclRingAllReduce::SetSwOverheadPerByteNs(uint64_t perByteNs) { m_swOverheadPerByteNs = perByteNs; }
void NcclRingAllReduce::SetLlThreshold(uint64_t threshold) { m_llThreshold = threshold; }
void NcclRingAllReduce::SetLl128Threshold(uint64_t threshold) { m_ll128Threshold = threshold; }

void
NcclRingAllReduce::SetComputeBaseDelays(Time ll, Time ll128, Time simple)
{
    m_computeBaseLLNs = ll.GetNanoSeconds();
    m_computeBaseLL128Ns = ll128.GetNanoSeconds();
    m_computeBaseSIMPLENs = simple.GetNanoSeconds();
}

uint64_t
NcclRingAllReduce::GetPerStepSwOverheadNs(uint64_t chunkSize) const
{
    if (m_swOverheadPerByteNs > 0 ||
        m_computeBaseLLNs > 0 || m_computeBaseLL128Ns > 0 || m_computeBaseSIMPLENs > 0)
    {
        uint64_t baseDelay;
        if (chunkSize < m_llThreshold)
            baseDelay = m_computeBaseLLNs;
        else if (chunkSize < m_ll128Threshold)
            baseDelay = m_computeBaseLL128Ns;
        else
            baseDelay = m_computeBaseSIMPLENs;
        return baseDelay + m_swOverheadPerByteNs * chunkSize;
    }
    return m_perStepSwOverheadNs;
}

void
NcclRingAllReduce::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
NcclRingAllReduce::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

CollectiveType NcclRingAllReduce::GetCollectiveType() const { return CollectiveType::ALLREDUCE; }
bool NcclRingAllReduce::IsCompleted() const { return m_completed; }

void
NcclRingAllReduce::DoDispose()
{
    // Detach the receive callbacks BEFORE dropping the endpoint references:
    // they hold raw `this`, so a packet still in flight after this injector
    // is disposed would invoke OnPacketReceived on freed memory (same class
    // of defect as codex round 16, finding 2). Ownership-checked (codex
    // round 20, finding 6): only the callback THIS injector installed is
    // cleared — a later injector on the same endpoints keeps its binding.
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->DetachReceiveCallbackIfOwner(
                MakeCallback(&NcclRingAllReduce::OnPacketReceived, this));
        }
    }
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

void
NcclRingAllReduce::Initialize(uint16_t numGpus, uint64_t dataSize,
                              const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    // Single-rank collective (codex round 21, finding 8): a one-rank ring
    // has no successor to pipeline to — the reduction of the rank's own
    // buffer is itself, so trivially complete instead of aborting.
    if (numGpus <= 1)
    {
        m_numGpus = numGpus;
        m_dataSize = dataSize;
        m_endpoints = endpoints;
        m_trivialSingleRank = true;
        return;
    }
    NS_ABORT_MSG_IF(numGpus < 2, "NcclRingAllReduce needs at least 2 ranks");
    NS_ABORT_MSG_IF(m_numNodes * m_gpusPerNode != numGpus,
                    "NcclRingAllReduce needs numGpus == numNodes * gpusPerNode "
                    "(node-major rank layout)");
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    m_numChannels = (m_numChannelsCfg > 0) ? m_numChannelsCfg : m_gpusPerNode;
    if (m_numChannels > m_gpusPerNode)
    {
        m_numChannels = m_gpusPerNode; // one crossing per rail at most
    }
    m_totalSteps = 2 * (numGpus - 1);
    // The (channel, step) pair is encoded into a 16-bit flow ID as
    // flowIdBase + channel*totalSteps + step (SendStepData), and the receiver
    // decodes it back with a divide/modulo. The space must actually fit:
    // at N=182 with C=182 channels the 65,884 encodings exceed the 65,535
    // usable IDs (base 1, 0 reserved as the "below base" reject sentinel),
    // so channel 181's step 13 wrapped to flow ID 0 — below m_flowIdBase —
    // and OnPacketReceived silently dropped every such chunk, stalling that
    // channel and the collective forever (codex round 26, finding 3).
    // Clamp the channel count (the ring already treats C as best-effort
    // parallelism — it is capped at gpusPerNode above for rail reasons):
    // C_max = (65535 - base + 1) / totalSteps, at least 1.
    {
        const uint64_t usableIds = 65536ull - m_flowIdBase;
        const uint64_t maxChannels = usableIds / m_totalSteps;
        if (static_cast<uint64_t>(m_numChannels) > maxChannels)
        {
            NS_LOG_WARN("NcclRingAllReduce: clamping channels " << m_numChannels
                        << " -> " << maxChannels
                        << ": (channel, step) pairs must fit the 16-bit flow-ID "
                           "space (base " << m_flowIdBase << ", "
                        << m_totalSteps << " steps)");
            m_numChannels = static_cast<uint32_t>(std::max<uint64_t>(maxChannels, 1));
        }
    }
    // The C*N chunk grid must COVER dataSize, not truncate it: plain integer
    // division drops dataSize % (C*N) bytes (2 nodes x 2 GPUs, 2 channels,
    // dataSize=15 -> chunkSize=1 -> only 8 of 15 tensor bytes represented,
    // yet the ring completes and reports the collective done — codex round
    // 12, finding 8). The tensor splits into C*N chunks in (channel, index)
    // row-major order; chunk i carries dataSize/chunkGrid bytes plus one
    // extra for the first dataSize % chunkGrid chunks, so the grid sums to
    // exactly dataSize. In the ring, rank r's step-s send moves chunk
    // (r - s) mod N of its channel (every step, all N chunks of the channel
    // advance one hop), so each chunk's size rides the wire 2(N-1) times and
    // total traffic is exactly 2(N-1) * dataSize — 90 B, not 48 B, at the
    // repro point. See ChunkSizeOf / ExpectedRecvBytes.
    const uint64_t chunkGrid = static_cast<uint64_t>(m_numChannels) * numGpus;
    m_chunkSize = dataSize / chunkGrid;
    m_chunkRemainder = dataSize % chunkGrid;
    if (m_chunkSize == 0 && dataSize > 0)
    {
        // D < C*N (nonzero): the grid has more slots than bytes. Keep the
        // legacy floor (1-byte chunks, no remainder) — a degenerate config
        // where NCCL itself would collapse channels; over-covering mirrors
        // the old behavior while still completing.
        m_chunkSize = 1;
        m_chunkRemainder = 0;
    }
    // Zero-data collective (codex round 22, finding 2): the degenerate
    // floor above fabricated 1-byte chunks at D=0, sending 2(N-1)*C*N
    // packets for a zero-byte operation (48 endpoint transmissions at
    // 2x2/2-channel). The frozen convention is trivial completion with
    // nothing on the wire.
    if (dataSize == 0)
    {
        m_chunkSize = 0;
        m_chunkRemainder = 0;
        m_trivialZeroData = true;
    }

    BuildRings();

    m_completedStep.assign(static_cast<size_t>(numGpus) * m_numChannels, 0);
    m_recvBytes.assign(static_cast<size_t>(numGpus) * m_numChannels, 0);
    m_doneChannels.assign(numGpus, 0);
    m_gpuDoneCount = 0;

    // Wire protocol from the per-step chunk size (LL/LL128/SIMPLE); startup
    // from the total data size — same convention as RingAllReduce.
    Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
    m_protocolId = protoModel->GetProtocolId(m_chunkSize);
    if (m_startupDelayNs == 0)
    {
        uint8_t startupProto = protoModel->GetProtocolId(m_dataSize);
        m_startupDelayNs = protoModel->GetStartupDelayNs(startupProto, m_numGpus);
    }

    SetupReceiveCallbacks();

    NS_LOG_INFO("NcclRingAllReduce initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize
                << " channels=" << m_numChannels
                << " chunkSize=" << m_chunkSize
                << " totalSteps=" << m_totalSteps
                << " protocol=" << static_cast<int>(m_protocolId)
                << " startupDelay=" << m_startupDelayNs << "ns");
}

// Channel c's ring visits node n's locals in the order c, c+1, ..., c+g-1
// (mod g); the last one crosses to node n+1's local c. Rotating the entry
// local per channel puts every channel's node crossing on a different GPU's
// NIC (rail spread) and every intra-node hop on the Su fabric.
void
NcclRingAllReduce::BuildRings()
{
    const uint32_t g = m_gpusPerNode;
    m_ringNext.assign(m_numChannels, std::vector<uint16_t>(m_numGpus, 0));
    m_ringPos.assign(m_numChannels, std::vector<uint32_t>(m_numGpus, 0));
    for (uint32_t c = 0; c < m_numChannels; ++c)
    {
        for (uint32_t node = 0; node < m_numNodes; ++node)
        {
            for (uint32_t k = 0; k < g; ++k)
            {
                uint16_t rank = static_cast<uint16_t>(node * g + (c + k) % g);
                uint16_t next;
                if (k + 1 < g)
                {
                    next = static_cast<uint16_t>(node * g + (c + k + 1) % g);
                }
                else
                {
                    next = static_cast<uint16_t>(((node + 1) % m_numNodes) * g + c % g);
                }
                m_ringNext[c][rank] = next;
            }
        }
        // Ring POSITION of each rank (0 at an arbitrary fixed start, following
        // ringNext around): rotated channels permute ranks, so chunk identity
        // must key on the position, not the numeric rank (codex round 13,
        // finding 4) -- channel 1 of 2 nodes x 2 GPUs orders 1->0->3->2, and
        // a rank-keyed grid hands rank 3 chunk 3 while its ring neighbours
        // exchange chunk boundaries that no longer line up.
        uint32_t pos = 0;
        uint16_t cur = 0;
        do
        {
            m_ringPos[c][cur] = pos;
            cur = m_ringNext[c][cur];
            pos++;
        } while (cur != 0 && pos < m_numGpus);
    }
}

void
NcclRingAllReduce::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&NcclRingAllReduce::OnPacketReceived, this));
        }
    }
}

void
NcclRingAllReduce::Start()
{
    m_running = true;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();
    // Zero-data short-circuit (codex round 22, finding 2): nothing to
    // reduce, nothing to send — trivial completion, no wire traffic.
    if (m_trivialZeroData)
    {
        NS_LOG_INFO("NcclRingAllReduce trivially complete (zero data)");
        Complete();
        return;
    }
    // Single-rank short-circuit (codex round 21, finding 8): no ring
    // successor, no peer receives — complete immediately.
    if (m_trivialSingleRank)
    {
        NS_LOG_INFO("NcclRingAllReduce trivially complete (single rank)");
        Complete();
        return;
    }
    NS_LOG_INFO("Starting NcclRingAllReduce with startup delay " << m_startupDelayNs << " ns");
    // Tracked in m_pendingEvents so DoDispose cancels it (raw-`this` target).
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(m_startupDelayNs),
                            &NcclRingAllReduce::TriggerInitialSends, this));
}

void
NcclRingAllReduce::TriggerInitialSends()
{
    // Every rank launches step 0 of every channel simultaneously; from here
    // on each (rank, channel) advances as its predecessor's chunks arrive.
    for (uint16_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        for (uint32_t c = 0; c < m_numChannels; ++c)
        {
            SendStepData(gpu, c, 0);
        }
    }
}

uint64_t
NcclRingAllReduce::ChunkSizeOf(uint32_t channel, uint16_t rank, uint32_t step) const
{
    // Row-major (channel, index) grid: channel c owns indices c*N .. c*N+N-1.
    // The chunk's identity keys on the RING POSITION, not the numeric rank:
    // rotated channels permute ranks (channel 1 of 2x2: 1->0->3->2), so the
    // rank at ring position p moves chunk (p - s) mod N at step s.
    uint32_t n = m_numGpus;
    uint32_t pos = m_ringPos[channel][rank];
    uint32_t idx = (pos + n - (step % n)) % n;
    uint64_t chunkIndex = static_cast<uint64_t>(channel) * n + idx;
    uint64_t size = m_chunkSize + (chunkIndex < m_chunkRemainder ? 1 : 0);
    return size;
}

uint64_t
NcclRingAllReduce::ExpectedRecvBytes(uint16_t destRank, uint32_t channel,
                                      uint32_t step) const
{
    // dest's ring predecessor for this channel is the rank whose ringNext is
    // destRank. Find it by scanning the channel's successor table (N <= a
    // few thousand; this runs once per completed step per (rank, channel)).
    for (uint16_t r = 0; r < m_numGpus; ++r)
    {
        if (m_ringNext[channel][r] == destRank)
        {
            return ChunkSizeOf(channel, r, step);
        }
    }
    return m_chunkSize; // unreachable: every rank has a successor
}

void
NcclRingAllReduce::SendStepData(uint16_t gpu, uint32_t channel, uint32_t step)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }
    uint16_t target = m_ringNext[channel][gpu];
    // Unique flowId per (channel, step) keeps sequence tracking per transfer
    // distinct (same reason RingAllReduce uses one flowId per step).
    uint16_t flowId = static_cast<uint16_t>(m_flowIdBase + channel * m_totalSteps + step);
    uint64_t chunk = ChunkSizeOf(channel, gpu, step);
    m_sentDataBytes += chunk;
    m_endpoints[gpu]->SendBulkWireTransferSize(target, chunk, m_protocolId, flowId, 0);
    NS_LOG_DEBUG("GPU " << gpu << " ch " << channel << " step " << step
                 << " -> GPU " << target << " (" << chunk << "B)");
}

void
NcclRingAllReduce::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    if (header.GetPacketType() == FabricPacketType::PERMANENT_LOSS)
    {
        // Permanently lost data must not count toward collective progress:
        // record and stall rather than complete with corrupt data.
        m_permanentLossCount++;
        NS_LOG_INFO("PERMANENT_LOSS: src=" << srcRank
                    << " dest=" << header.GetDestRank());
        return;
    }
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t destRank = header.GetDestRank();
    uint16_t flowId = header.GetFlowId();
    if (destRank >= m_numGpus || flowId < m_flowIdBase)
    {
        // A flow ID below base is either foreign traffic sharing the callback
        // or a (channel, step) encoding that wrapped past the 16-bit space —
        // Initialize clamps the channel count so our own encodings never
        // land here (codex round 26, finding 3), but a wrapped ID must never
        // silently stall a channel again: log it so a regression is visible.
        NS_LOG_WARN("NcclRingAllReduce: dropping DATA with flowId " << flowId
                    << " (< base " << m_flowIdBase << ") from rank " << srcRank);
        return;
    }
    uint32_t channel = (flowId - m_flowIdBase) / m_totalSteps;
    if (channel >= m_numChannels)
    {
        return;
    }

    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    size_t idx = static_cast<size_t>(destRank) * m_numChannels + channel;
    m_recvBytes[idx] += effectiveSize;

    // Chunks of consecutive steps can interleave on the wire; subtracting one
    // chunk per completed step (instead of resetting) keeps the carry-over.
    // The expected size is the SENDER's step chunk (dest receives what its
    // ring predecessor sent for that step), tracked in m_expectedBytes
    // alongside m_completedStep.
    while (m_completedStep[idx] < m_totalSteps &&
           m_recvBytes[idx] >= ExpectedRecvBytes(destRank, channel, m_completedStep[idx]))
    {
        m_recvBytes[idx] -= ExpectedRecvBytes(destRank, channel, m_completedStep[idx]);
        uint32_t step = m_completedStep[idx]++;
        OnStepReceiveComplete(destRank, channel, step);
    }
}

void
NcclRingAllReduce::OnStepReceiveComplete(uint16_t gpu, uint32_t channel, uint32_t step)
{
    if (step + 1 >= m_totalSteps)
    {
        // This (rank, channel) pipeline is drained.
        if (++m_doneChannels[gpu] >= m_numChannels)
        {
            m_gpuDoneCount++;
            LogStepComplete(gpu, m_totalSteps - 1, m_startTimeNs,
                            Simulator::Now().GetNanoSeconds());
            NS_LOG_INFO("GPU " << gpu << " DONE ("
                        << m_gpuDoneCount << "/" << m_numGpus << ")");
            if (m_gpuDoneCount >= m_numGpus)
            {
                Complete();
            }
        }
        return;
    }

    uint64_t swOverhead = GetPerStepSwOverheadNs(m_chunkSize);
    if (swOverhead > 0)
    {
        m_pendingEvents.push_back(
            Simulator::Schedule(NanoSeconds(swOverhead),
                                &NcclRingAllReduce::SendStepData, this, gpu, channel, step + 1));
    }
    else
    {
        m_pendingEvents.push_back(
            Simulator::ScheduleNow(&NcclRingAllReduce::SendStepData, this, gpu, channel, step + 1));
    }
}

void
NcclRingAllReduce::Complete()
{
    m_completed = true;
    m_running = false;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();
    uint64_t durationNs = m_endTimeNs - m_startTimeNs;
    NS_LOG_INFO("NcclRingAllReduce COMPLETE in " << durationNs / 1000.0 << " µs");
    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }
}

} // namespace ns3
