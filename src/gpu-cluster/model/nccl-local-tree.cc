// SPDX-License-Identifier: GPL-2.0-only
#include "nccl-local-tree.h"

#include "nccl-protocol.h"
#include "tree-allreduce.h"

#include "ns3/abort.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <limits>
#include <numeric>

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(NcclLocalTree);

namespace
{
uint64_t
CeilDiv(uint64_t bytes, uint64_t unit)
{
    return bytes / unit + (bytes % unit != 0);
}
} // namespace

TypeId
NcclLocalTree::GetTypeId()
{
    static TypeId tid = TypeId("ns3::NcclLocalTree")
                            .SetParent<CollectiveInjector>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<NcclLocalTree>();
    return tid;
}

void
NcclLocalTree::Configure(uint32_t channelLimit,
                         uint64_t simpleBuffer,
                         Time hopOverhead,
                         uint64_t overheadNsPerByte)
{
    NS_ABORT_MSG_IF(channelLimit > MaxChannels, "treeMaxChannels exceeds NCCL MAXCHANNELS (64)");
    // One SIMPLE step must contain at least one full 512-byte protocol grain.
    NS_ABORT_MSG_IF(
        simpleBuffer < Steps * 512 || simpleBuffer > std::numeric_limits<int>::max(),
        "treeSimpleBufferBytes must fit NCCL's buffer and contain a SIMPLE grain per step");
    m_channelLimit = channelLimit ? channelLimit : 2;
    m_simpleBuffer = simpleBuffer;
    NS_ABORT_MSG_IF(hopOverhead.GetNanoSeconds() < 0, "Tree hop overhead must be nonnegative");
    m_hopNs = hopOverhead.GetNanoSeconds();
    m_byteNs = overheadNsPerByte;
}

void
NcclLocalTree::BuildChannelGroups(uint64_t bytes)
{
    m_channelBytes.clear();
    m_groups.clear();
    if (bytes == 0)
    {
        return;
    }
    // NCCL v2.27.7 tuning.cc + comm.h default Tree thread thresholds and
    // enqueue.cc topoGetAlgoInfo/scheduleCollTasksToPlan for an isolated op.
    constexpr uint64_t threshold = 512 * 64; // SIMPLE threads * bytes per thread
    const uint64_t budget = std::clamp<uint64_t>(bytes / threshold, 1, m_channelLimit);
    // 16 KiB minimum traffic per cell; AllReduce moves twice the payload.
    constexpr uint64_t trafficFactor = 2;
    constexpr uint64_t cell = (16ULL << 10) / trafficFactor;
    // floor(bytes * trafficFactor / budget), without multiplying the full size.
    const uint64_t share = (bytes / budget) / cell;
    const uint64_t rem =
        ((bytes / budget) % cell) * trafficFactor + (bytes % budget) * trafficFactor / budget;
    const uint64_t cellsPerChannel = std::max<uint64_t>(1, share + (rem != 0));
    const uint64_t cells = CeilDiv(bytes, cell);
    // Integer division of traffic can leave a partial cell for one more
    // channel. The communicator limit bounds the final range assignment.
    const uint64_t count = std::min<uint64_t>(m_channelLimit, CeilDiv(cells, cellsPerChannel));
    uint64_t remaining = bytes;
    for (uint64_t i = 0; i < count; ++i)
    {
        // The last range can contain part of a cell.
        const uint64_t part = i + 1 == count ? remaining : cellsPerChannel * cell;
        remaining -= part;
        const uint64_t chunk = GetChunkBytes();
        m_channelBytes.push_back(part);
        if (!m_groups.empty() && m_groups.back().bytes == part)
        {
            ++m_groups.back().channels;
        }
        else
        {
            m_groups.push_back(
                {part, chunk, CeilDiv(part, chunk), 1, std::vector<Stage>(m_path.size() - 1)});
        }
    }
}

void
NcclLocalTree::Initialize(uint16_t ranks,
                          uint64_t bytes,
                          const std::vector<Ptr<FabricEndpoint>>& endpoints)
{
    NS_ABORT_MSG_IF(!ranks || endpoints.size() != ranks, "Tree requires a complete local group");
    for (const auto& ep : endpoints)
    {
        NS_ABORT_MSG_IF(!ep || ep->GetNodeId() != endpoints.front()->GetNodeId(),
                        "NcclLocalTree requires ranks on the same host");
    }
    m_endpoints = endpoints;
    m_order.clear();
    m_position.assign(ranks, UINT16_MAX);
    uint16_t rank = 0;
    while (rank < ranks && m_position[rank] == UINT16_MAX)
    {
        m_position[rank] = m_order.size();
        m_order.push_back(rank);
        rank = endpoints[rank]->GetRingNext();
    }
    // Use the installed locality order when it covers this communicator.
    if (m_order.size() != ranks || rank != 0)
    {
        m_order.resize(ranks);
        std::iota(m_order.begin(), m_order.end(), 0);
        m_position = m_order;
    }
    m_path.assign(m_order.rbegin(), m_order.rend());
    m_path.insert(m_path.end(), m_order.begin() + 1, m_order.end());
    auto protocol = endpoints.front()->GetProtocolModel();
    // Keep the simulator's existing Tree size heuristic for the wire protocol.
    m_protocol = SelectTreeWireProtocol(protocol, bytes, ranks);
    NS_ABORT_MSG_IF(m_protocol != static_cast<uint8_t>(NcclProtocol::SIMPLE),
                    "NcclLocalTree models SIMPLE step readiness");
    m_startupNs = m_startupOverride.value_or(protocol->GetStartupDelayNs(m_protocol, ranks));
    BuildChannelGroups(bytes);
    m_groupsDone.assign(m_path.size() - 1, 0);
    m_delivered.assign(m_path.size() - 1, 0);
    m_nextFreeNs.assign(m_path.size() - 1, std::nullopt);
    m_done = false;
    for (auto& ep : m_endpoints)
    {
        ep->SetReceiveCallback(MakeCallback(&NcclLocalTree::Receive, this));
    }
}

uint64_t
NcclLocalTree::ChannelGroup::BytesAt(uint64_t iteration) const
{
    return std::min(chunk, bytes - iteration * chunk);
}

void
NcclLocalTree::SetStartupDelay(Time delay)
{
    m_startupNs = delay.GetNanoSeconds();
    m_startupOverride = m_startupNs;
}

void
NcclLocalTree::Start()
{
    m_startNs = Simulator::Now().GetNanoSeconds();
    if (m_groups.empty())
    {
        Finish();
        return;
    }
    m_pendingEvents.push_back(
        Simulator::Schedule(NanoSeconds(m_startupNs), &NcclLocalTree::Launch, this));
}

void
NcclLocalTree::Launch()
{
    if (m_order.size() == 1)
    {
        Finish();
        return;
    }
    for (size_t i = 0; i < m_groups.size(); ++i)
    {
        Pump(i, 0);
    }
}

void
NcclLocalTree::Pump(size_t groupId, size_t hop)
{
    auto& group = m_groups[groupId];
    auto& stage = group.stages[hop];
    const uint64_t ready = hop == 0 ? group.iterations : group.stages[hop - 1].completed;
    while (stage.issued < ready && stage.issued - stage.completed < Steps)
    {
        const uint64_t iteration = stage.issued++;
        // A slot is reused only after delivery on this directed edge. Packets
        // from later chunks cannot release an earlier dependency after spray.
        const uint16_t flow = 1 + groupId * Steps + iteration % Steps;
        const uint64_t part = group.BytesAt(iteration);
        const uint64_t now = Simulator::Now().GetNanoSeconds();
        auto& nextFree = m_nextFreeNs[hop];
        NS_ABORT_MSG_IF(!nextFree && m_hopNs > INT64_MAX - now,
                        "Tree hop start exceeds simulation time");
        const uint64_t start = nextFree ? std::max(now, *nextFree) : now + m_hopNs;
        const uint64_t payload = part * group.channels;
        NS_ABORT_MSG_IF(m_byteNs && payload > (INT64_MAX - start) / m_byteNs,
                        "Tree software service exceeds the simulation time range");
        const uint64_t sendAt = start + payload * m_byteNs;
        nextFree = sendAt;
        if (sendAt == now)
        {
            Send(m_path[hop], m_path[hop + 1], flow, part, group.channels);
        }
        else
        {
            m_pendingEvents.push_back(Simulator::Schedule(NanoSeconds(sendAt - now),
                                                          &NcclLocalTree::Send,
                                                          this,
                                                          m_path[hop],
                                                          m_path[hop + 1],
                                                          flow,
                                                          part,
                                                          group.channels));
        }
    }
}

void
NcclLocalTree::Send(uint16_t source,
                    uint16_t dest,
                    uint16_t flow,
                    uint64_t bytes,
                    uint32_t channels)
{
    // Keep protocol framing and packet striping per channel; all submissions
    // enter the same endpoint and compete for the physical link queues.
    for (uint32_t c = 0; c < channels; ++c)
    {
        m_endpoints[source]->SendBulkWireTransferSize(dest, bytes, m_protocol, flow, 0);
    }
}

void
NcclLocalTree::Receive(uint16_t dest, Ptr<Packet> packet, FabricHeader header)
{
    const uint16_t sender = header.GetSourceRank();
    const uint16_t flow = header.GetFlowId();
    if (m_done || !flow || sender >= m_order.size() ||
        header.GetPacketType() != FabricPacketType::DATA || dest >= m_order.size() ||
        header.GetDestRank() != dest)
    {
        return;
    }
    const size_t id = (flow - 1) / Steps;
    if (id >= m_groups.size())
    {
        return;
    }
    const size_t from = m_position[sender], to = m_position[dest], depth = m_order.size() - 1;
    if (from + 1 != to && to + 1 != from)
    {
        return;
    }
    const size_t hop = from > to ? depth - from : depth + from;
    auto& group = m_groups[id];
    auto& stage = group.stages[hop];
    const size_t slot = (flow - 1) % Steps;
    const size_t offset = (slot + Steps - stage.completed % Steps) % Steps;
    if (offset >= stage.issued - stage.completed)
    {
        return; // no outstanding transfer for this slot on this directed edge
    }
    const uint64_t payload =
        header.GetEffectiveDataSize() ? header.GetEffectiveDataSize() : packet->GetSize();
    stage.received[slot] += payload;
    m_delivered[hop] += payload;
    const uint64_t before = stage.completed;
    while (stage.completed < stage.issued)
    {
        auto& received = stage.received[stage.completed % Steps];
        const uint64_t expected = group.BytesAt(stage.completed) * group.channels;
        if (received < expected)
        {
            break;
        }
        NS_ABORT_MSG_IF(received != expected, "Tree received excess payload for a FIFO slot");
        received = 0;
        ++stage.completed;
    }
    if (stage.completed == before)
    {
        return;
    }
    if (hop + 1 < group.stages.size())
    {
        Pump(id, hop + 1);
    }
    if (stage.completed == group.iterations)
    {
        if (++m_groupsDone[hop] == m_groups.size())
        {
            LogStepComplete(dest, hop, m_startNs, Simulator::Now().GetNanoSeconds());
            if (hop + 1 == group.stages.size())
            {
                Finish();
            }
        }
    }
    else
    {
        Pump(id, hop);
    }
}

void
NcclLocalTree::Finish()
{
    m_done = true;
    if (m_complete)
    {
        m_complete(Simulator::Now().GetNanoSeconds() - m_startNs);
    }
}

void
NcclLocalTree::DoDispose()
{
    for (auto& ep : m_endpoints)
    {
        ep->DetachReceiveCallbackIfOwner(MakeCallback(&NcclLocalTree::Receive, this));
    }
    m_endpoints.clear();
    m_complete = nullptr;
    CollectiveInjector::DoDispose();
}

} // namespace ns3
