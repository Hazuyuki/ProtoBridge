/*
 * SPDX-License-Identifier: GPL-2.0-only
 * tree-allreduce.cc
 *
 * Chunk-pipelined Binary Tree AllReduce carrying the FULL data size D over
 * every tree edge in each direction (see tree-allreduce.h for the volume
 * model).
 *
 * Mechanics:
 * - Heap-arithmetic balanced binary tree (parent = (g-1)/2).
 * - Reduce (up) flow: leaves send D to their parents at start; an interior
 *   GPU forwards the byte range [sent, min-over-children-received) up.  The
 *   root's reduce progress doubles as the broadcast source.
 * - Broadcast (down) flow: the root sends reduced byte ranges to both
 *   children; interior GPUs forward what arrived from their parent.
 * - Forwarding is quantized to m_pipeChunkSize = clamp(D/(32*depth), 64 KiB,
 *   8 MiB): uniform per-hop granularity (no re-fragmentation cascade down
 *   the tree), pipeline-fill error <= ~(2*depth)*chunk/D ~ 6%, bounded event
 *   count.  Sub-chunk tails are held until the flow end.
 * - Progress is byte-counted per (flow, edge); packet order never matters.
 * - Software overhead knobs pace each hop: perStep once at the flow's first
 *   chunk + perByte as a rate tax (both default 0 => zero extra events).
 */

#include "tree-allreduce.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "nccl-protocol.h"
#include "protocol-model.h"

#include <algorithm>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("TreeAllReduce");

NS_OBJECT_ENSURE_REGISTERED(TreeAllReduce);

namespace
{
constexpr uint16_t NO_RANK = 0xFFFF;
constexpr uint16_t FLOW_UP = 1;    // reduce direction (child -> parent)
constexpr uint16_t FLOW_DOWN = 2;  // broadcast direction (parent -> child)
constexpr uint64_t PIPE_CHUNK_MIN = 64ULL * 1024;
constexpr uint64_t PIPE_CHUNK_MAX = 8ULL * 1024 * 1024;
} // namespace

TypeId
TreeAllReduce::GetTypeId()
{
    static TypeId tid = TypeId("ns3::TreeAllReduce")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<TreeAllReduce>();
    return tid;
}

TreeAllReduce::TreeAllReduce()
    : m_numGpus(0),
      m_dataSize(0),
      m_chunkSize(0),
      m_pipeChunkSize(0),
      m_flowId(1),
      m_protocolId(0),
      m_depth(0),
      m_totalSteps(0),
      m_state(TreeAllReduceState::IDLE),
      m_startupDelayNs(0),
      m_perStepSwOverheadNs(0),
      m_swOverheadPerByteNs(0),
      m_gpuDoneCount(0),
      m_startTimeNs(0),
      m_endTimeNs(0)
{
}

TreeAllReduce::~TreeAllReduce()
{
}

void
TreeAllReduce::DoDispose()
{
    m_endpoints.clear();
    m_completionCallback = nullptr;
    CollectiveInjector::DoDispose();
}

uint16_t
TreeAllReduce::GetParent(uint16_t gpu) const
{
    if (gpu == NO_RANK)
    {
        return NO_RANK;
    }
    // Heap-arithmetic binary tree (correct, balanced depth=log2(N)).
    // NOTE: the G3.1 topology-embedded parent (m_endpoints[gpu]->GetTreeParent())
    // is not used here because it produces a degenerate tree on several
    // topologies (e.g. switched: depth=5 for N=4, zero matched senders),
    // which makes the collective complete at startup with no wire time.
    if (gpu == 0) return NO_RANK;
    return (gpu - 1) / 2;
}

uint16_t
TreeAllReduce::GetLeftChild(uint16_t gpu) const
{
    uint16_t child = 2 * gpu + 1;
    return (child < m_numGpus) ? child : NO_RANK;
}

uint16_t
TreeAllReduce::GetRightChild(uint16_t gpu) const
{
    uint16_t child = 2 * gpu + 2;
    return (child < m_numGpus) ? child : NO_RANK;
}

uint16_t
TreeAllReduce::GetTreeDepth() const
{
    // Heap-arithmetic balanced binary tree: depth = ceil(log2(N)).
    // (round up so non-power-of-2 N still reaches every GPU.)
    uint16_t depth = 0;
    uint32_t n = m_numGpus;
    while (n > 1)
    {
        n = (n + 1) / 2;
        depth++;
    }
    return depth ? depth : 1;
}

uint16_t
TreeAllReduce::GetTreeLevel(uint16_t gpu) const
{
    // Walk parents to the root. Guards against cycles by capping the walk.
    uint16_t level = 0;
    uint16_t g = gpu;
    uint16_t guard = 0;
    while (g != NO_RANK && GetParent(g) != NO_RANK && guard < m_numGpus + 1)
    {
        g = GetParent(g);
        level++;
        guard++;
    }
    return level;
}

void
TreeAllReduce::Initialize(uint16_t numGpus, uint64_t dataSize,
                           const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    m_numGpus = numGpus;
    m_dataSize = dataSize;
    m_endpoints = endpoints;

    m_depth = GetTreeDepth();
    m_totalSteps = 2 * m_depth;

    // D/N, kept only for protocol-threshold selection (same per-step payload
    // granularity the ring model uses to pick LL/LL128/SIMPLE).
    m_chunkSize = dataSize / numGpus;
    if (m_chunkSize == 0) m_chunkSize = dataSize;

    m_pipeChunkSize = std::clamp<uint64_t>(dataSize / (32ULL * m_depth),
                                           PIPE_CHUNK_MIN, PIPE_CHUNK_MAX);

    m_upRecvFromChild.assign(numGpus, 0);
    m_upSent.assign(numGpus, 0);
    m_downRecv.assign(numGpus, 0);
    m_downSent.assign(numGpus, 0);
    m_upNextFreeNs.assign(numGpus, 0);
    m_downNextFreeNs.assign(numGpus, 0);
    m_upDone.assign(numGpus, false);
    m_downDone.assign(numGpus, false);
    m_gpuCurrentStep.assign(numGpus, 0);
    m_gpuDoneCount = 0;

    m_flowId = 1;

    // Protocol selection based on chunk size (per-step payload)
    Ptr<ProtocolModel> protoModel = m_endpoints[0]->GetProtocolModel();
    if (m_startupDelayNs == 0)
    {
        uint8_t protoId = protoModel->GetProtocolId(dataSize);
        m_startupDelayNs = protoModel->GetStartupDelayNs(protoId, m_numGpus);
    }
    NcclProtocol wireProto = static_cast<NcclProtocol>(protoModel->GetProtocolId(m_chunkSize));
    m_protocolId = static_cast<uint8_t>(wireProto);

    SetupReceiveCallbacks();

    NS_LOG_INFO("Pipelined TreeAllReduce initialized: numGpus=" << numGpus
                << " dataSize=" << dataSize << " pipeChunk=" << m_pipeChunkSize
                << " depth=" << m_depth
                << " startupDelay=" << m_startupDelayNs << "ns"
                << " protocol=" << static_cast<int>(wireProto));
}

void
TreeAllReduce::SetupReceiveCallbacks()
{
    for (auto& ep : m_endpoints)
    {
        if (ep)
        {
            ep->SetReceiveCallback(
                MakeCallback(&TreeAllReduce::OnPacketReceived, this));
        }
    }
}

void
TreeAllReduce::SetCompletionCallback(std::function<void(uint64_t durationNs)> cb)
{
    m_completionCallback = cb;
}

void
TreeAllReduce::SetStartupDelay(Time delay)
{
    m_startupDelayNs = delay.GetNanoSeconds();
}

void
TreeAllReduce::SetPerStepSwOverhead(Time delay)
{
    m_perStepSwOverheadNs = delay.GetNanoSeconds();
}

void
TreeAllReduce::SetSwOverheadPerByteNs(uint64_t nsPerByte)
{
    m_swOverheadPerByteNs = nsPerByte;
}

void
TreeAllReduce::Start()
{
    m_state = TreeAllReduceState::RUNNING;
    m_startTimeNs = Simulator::Now().GetNanoSeconds();

    NS_LOG_INFO("Starting Pipelined Tree AllReduce with startup delay " << m_startupDelayNs << " ns");

    Simulator::Schedule(NanoSeconds(m_startupDelayNs), &TreeAllReduce::TriggerInitialSends, this);
}

void
TreeAllReduce::TriggerInitialSends()
{
    if (m_numGpus <= 1)
    {
        m_gpuDoneCount = m_numGpus;
        Complete();
        return;
    }
    // Leaves have their full contribution locally: start their reduce flows.
    for (uint16_t gpu = 1; gpu < m_numGpus; gpu++)
    {
        if (GetLeftChild(gpu) == NO_RANK && GetRightChild(gpu) == NO_RANK)
        {
            PumpUp(gpu);
        }
    }
}

uint64_t
TreeAllReduce::UpReadyBytes(uint16_t gpu) const
{
    uint16_t left = GetLeftChild(gpu);
    uint16_t right = GetRightChild(gpu);
    uint64_t ready = m_dataSize;  // own contribution is local
    if (left != NO_RANK)
    {
        ready = std::min(ready, m_upRecvFromChild[left]);
    }
    if (right != NO_RANK)
    {
        ready = std::min(ready, m_upRecvFromChild[right]);
    }
    return ready;
}

uint64_t
TreeAllReduce::PacedSendDelayNs(uint64_t& nextFreeNs, bool firstOfFlow, uint64_t bytes)
{
    uint64_t now = Simulator::Now().GetNanoSeconds();
    uint64_t sendAt = std::max(now, nextFreeNs);
    if (firstOfFlow)
    {
        sendAt += m_perStepSwOverheadNs;
    }
    sendAt += m_swOverheadPerByteNs * bytes;
    nextFreeNs = sendAt;
    return sendAt - now;
}

void
TreeAllReduce::DoSend(uint16_t gpu, uint16_t target, uint64_t bytes, uint16_t flowId)
{
    if (gpu >= m_endpoints.size() || !m_endpoints[gpu])
    {
        return;
    }
    m_endpoints[gpu]->SendBulkWireTransferSize(target, bytes, m_protocolId, flowId, 0);
}

void
TreeAllReduce::ScheduleOrSend(uint16_t gpu, uint16_t target, uint64_t bytes,
                              uint16_t flowId, uint64_t delayNs)
{
    NS_LOG_DEBUG("GPU " << gpu << " -> " << target << " " << bytes << " bytes ("
                 << (flowId == FLOW_UP ? "REDUCE" : "BROADCAST")
                 << " delay=" << delayNs << "ns)");
    if (delayNs == 0)
    {
        DoSend(gpu, target, bytes, flowId);
    }
    else
    {
        Simulator::Schedule(NanoSeconds(delayNs), &TreeAllReduce::DoSend, this,
                            gpu, target, bytes, flowId);
    }
}

void
TreeAllReduce::PumpUp(uint16_t gpu)
{
    uint16_t parent = GetParent(gpu);
    if (parent == NO_RANK)
    {
        return;  // the root's reduce progress is drained by PumpDown
    }
    uint64_t ready = UpReadyBytes(gpu);
    while (m_upSent[gpu] < ready)
    {
        uint64_t remaining = ready - m_upSent[gpu];
        if (remaining < m_pipeChunkSize && ready < m_dataSize)
        {
            break;  // hold the sub-chunk tail until more bytes (or flow end)
        }
        uint64_t chunk = std::min(remaining, m_pipeChunkSize);
        uint64_t delay = PacedSendDelayNs(m_upNextFreeNs[gpu], m_upSent[gpu] == 0, chunk);
        ScheduleOrSend(gpu, parent, chunk, FLOW_UP, delay);
        m_upSent[gpu] += chunk;
    }
}

void
TreeAllReduce::PumpDown(uint16_t gpu)
{
    uint16_t left = GetLeftChild(gpu);
    uint16_t right = GetRightChild(gpu);
    if (left == NO_RANK && right == NO_RANK)
    {
        return;  // leaf: nothing to forward
    }
    // The root re-broadcasts its reduce progress; interior GPUs forward what
    // arrived from their parent.
    uint64_t ready = (gpu == 0) ? UpReadyBytes(0)
                                : std::min(m_downRecv[gpu], m_dataSize);
    while (m_downSent[gpu] < ready)
    {
        uint64_t remaining = ready - m_downSent[gpu];
        if (remaining < m_pipeChunkSize && ready < m_dataSize)
        {
            break;
        }
        uint64_t chunk = std::min(remaining, m_pipeChunkSize);
        uint64_t delay = PacedSendDelayNs(m_downNextFreeNs[gpu], m_downSent[gpu] == 0, chunk);
        ScheduleOrSend(gpu, left, chunk, FLOW_DOWN, delay);
        if (right != NO_RANK)
        {
            ScheduleOrSend(gpu, right, chunk, FLOW_DOWN, delay);
        }
        m_downSent[gpu] += chunk;
    }
}

void
TreeAllReduce::OnPacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    // NOTE: the callback's first argument is the RECEIVING endpoint's rank
    // (FabricEndpoint passes m_rank), so the sender is read from the header.
    if (header.GetPacketType() != FabricPacketType::DATA)
    {
        return;
    }

    uint16_t sender = header.GetSourceRank();
    uint16_t destRank = header.GetDestRank();
    if (destRank >= m_numGpus || m_state != TreeAllReduceState::RUNNING)
    {
        return;
    }

    uint64_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = packet->GetSize();
    }

    uint64_t nowNs = Simulator::Now().GetNanoSeconds();

    if (sender == GetParent(destRank))
    {
        // Broadcast direction: parent -> this GPU.
        m_downRecv[destRank] += effectiveSize;
        PumpDown(destRank);
        if (!m_downDone[destRank] && m_downRecv[destRank] >= m_dataSize)
        {
            m_downDone[destRank] = true;
            // Old global-schedule step index of this GPU's broadcast receive.
            LogStepComplete(destRank, m_depth + GetTreeLevel(destRank) - 1,
                            m_startTimeNs, nowNs);
            MarkGpuDone(destRank);
        }
    }
    else if (sender == GetLeftChild(destRank) || sender == GetRightChild(destRank))
    {
        // Reduce direction: child -> this GPU.
        m_upRecvFromChild[sender] += effectiveSize;
        if (destRank == 0)
        {
            PumpDown(0);
        }
        else
        {
            PumpUp(destRank);
        }
        if (!m_upDone[destRank] && UpReadyBytes(destRank) >= m_dataSize)
        {
            m_upDone[destRank] = true;
            m_gpuCurrentStep[destRank] = m_depth;
            // Old global-schedule step index of this GPU's reduce receive.
            LogStepComplete(destRank, m_depth - 1 - GetTreeLevel(destRank),
                            m_startTimeNs, nowNs);
            if (destRank == 0)
            {
                MarkGpuDone(0);  // root: no broadcast receive, done at reduce end
            }
        }
    }
    // else: not a tree-adjacent sender for this GPU -- ignore.
}

void
TreeAllReduce::MarkGpuDone(uint16_t gpu)
{
    m_gpuCurrentStep[gpu] = m_totalSteps;
    m_gpuDoneCount++;
    NS_LOG_INFO("GPU " << gpu << " DONE (total done=" << m_gpuDoneCount << "/" << m_numGpus << ")");
    CheckGlobalCompletion();
}

void
TreeAllReduce::CheckGlobalCompletion()
{
    if (m_gpuDoneCount >= m_numGpus)
    {
        Complete();
    }
}

CollectiveType
TreeAllReduce::GetCollectiveType() const
{
    return CollectiveType::ALLREDUCE;
}

bool
TreeAllReduce::IsCompleted() const
{
    return m_state == TreeAllReduceState::COMPLETED;
}

void
TreeAllReduce::Complete()
{
    m_state = TreeAllReduceState::COMPLETED;
    m_endTimeNs = Simulator::Now().GetNanoSeconds();

    uint64_t durationNs = m_endTimeNs - m_startTimeNs;

    NS_LOG_INFO("Pipelined Tree AllReduce COMPLETE in " << durationNs / 1000.0 << " µs");

    if (m_completionCallback)
    {
        m_completionCallback(durationNs);
    }

}

} // namespace ns3
