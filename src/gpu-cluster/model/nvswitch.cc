/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * NVSwitch Hardware Model Implementation
 */

#include "nvswitch.h"

#include <algorithm>

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include "ns3/uinteger.h"
#include "ns3/boolean.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/ppp-header.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("NvSwitch");

NS_OBJECT_ENSURE_REGISTERED(NvSwitch);

TypeId
NvSwitch::GetTypeId()
{
    static TypeId tid = TypeId("ns3::NvSwitch")
        .SetParent<FabricSwitch>()
        .SetGroupName("GpuCluster")
        .AddConstructor<NvSwitch>()
        .AddAttribute("VoqDepth",
                      "Maximum number of packets in each VOQ",
                      UintegerValue(64),
                      MakeUintegerAccessor(&NvSwitch::m_voqDepth),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("ArbitrationInterval",
                      "Arbitration interval in nanoseconds",
                      UintegerValue(100),
                      MakeUintegerAccessor(&NvSwitch::m_arbitrationIntervalNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("AllReduceEnabled",
                      "Enable All-Reduce aggregation in switch",
                      BooleanValue(false),
                      MakeBooleanAccessor(&NvSwitch::m_allReduceEnabled),
                      MakeBooleanChecker())
        .AddAttribute("AllReduceThreshold",
                      "Number of GPUs that must contribute before All-Reduce aggregation",
                      UintegerValue(8),
                      MakeUintegerAccessor(&NvSwitch::m_allReduceThreshold),
                      MakeUintegerChecker<uint32_t>())
        .AddAttribute("AllReduceAggregationDelay",
                      "In-switch reduction latency in nanoseconds (SHARP aggregation delay)",
                      UintegerValue(500),
                      MakeUintegerAccessor(&NvSwitch::m_allReduceAggregationDelayNs),
                      MakeUintegerChecker<uint64_t>())
        .AddAttribute("AllReduceDataSize",
                      "Expected data size per GPU for SHARP AllReduce (bytes)",
                      UintegerValue(0),
                      MakeUintegerAccessor(&NvSwitch::m_allReduceDataSize),
                      MakeUintegerChecker<uint64_t>())
        .AddTraceSource("Tx",
                        "A packet is transmitted",
                        MakeTraceSourceAccessor(&NvSwitch::m_txTrace),
                        "ns3::Packet::TracedCallback")
        .AddTraceSource("Rx",
                        "A packet is received",
                        MakeTraceSourceAccessor(&NvSwitch::m_rxTrace),
                        "ns3::Packet::TracedCallback")
        .AddTraceSource("Drop",
                        "A packet is dropped",
                        MakeTraceSourceAccessor(&NvSwitch::m_dropTrace),
                        "ns3::Packet::TracedCallback");
    return tid;
}

NvSwitch::NvSwitch()
    : m_ifIndex(0),
      m_mtu(9000),
      m_sourceBasedRoutingEnabled(false),
      m_sprayRoutingEnabled(false),
      m_failureAwareRoutingEnabled(false),
      m_voqDepth(64),
      m_arbitrationIntervalNs(100),
      m_currentArbitrationPort(0),
      m_allReduceEnabled(false),
      m_allReduceThreshold(8),
      m_allReduceAggregationDelayNs(500),
      m_allReduceDataSize(0),
      m_allReduceNumPartitions(0),
      m_allGatherEnabled(false),
      m_allGatherThreshold(8),
      m_allGatherChunkSize(0),
      m_allGatherDataSize(0),
      m_txBytes(0),
      m_rxBytes(0),
      m_txPackets(0),
      m_rxPackets(0),
      m_droppedPackets(0),
      m_unknownPortDrops(0),
      m_linkErrorDrops(0),
      m_ttlDrops(0),
      m_voqDrops(0),
      m_routeUnavailableDrops(0)
{
    NS_LOG_FUNCTION(this);
    // Default arbitration strategy: the historical non-blocking crossbar.
    // A config-provided "Arbiter" attribute or SetArbiter() overrides this.
    if (!m_arbiter)
    {
        m_arbiter = CreateObject<RoundRobinArbiter>();
    }
}

NvSwitch::~NvSwitch()
{
    NS_LOG_FUNCTION(this);
}

void
NvSwitch::DoDispose()
{
    NS_LOG_FUNCTION(this);
    // Cancel every pending deferred-result event: each targets raw `this`
    // (bound member call or lambda capture) and dereferences the buffers and
    // ports cleared below — a survivor fired after disposal crashed in
    // MulticastAllReduceResult (codex round 15, finding 2, valgrind-confirmed
    // UAF/SIGSEGV after disposing the switch mid-aggregation-delay).
    for (const EventId& ev : m_deferredResultEvents)
    {
        if (!ev.IsExpired())
        {
            Simulator::Cancel(ev);
        }
    }
    m_deferredResultEvents.clear();
    // Detach the port promisc callbacks BEFORE clearing the port list: they
    // hold raw `this`, so a packet arriving on any port channel after this
    // switch is gone would invoke ReceiveFromPort on freed memory (codex
    // round 16, finding 2 — a 64-B packet over a 100-us link, with the
    // receiving switch disposed and released at 1 us, produced
    // valgrind-confirmed invalid reads AND writes at the 100-us arrival).
    for (const auto& port : m_ports)
    {
        if (port)
        {
            port->SetPromiscReceiveCallback(
                MakeNullCallback<bool, Ptr<NetDevice>, Ptr<const Packet>, uint16_t, const Address&, const Address&, NetDevice::PacketType>());
        }
    }
    m_ports.clear();
    m_voqs.clear();
    m_portDegradationModels.clear();
    m_macTable.clear();
    m_sourceBasedTable.clear();
    m_sprayPortsTable.clear();
    m_sprayRoundRobin.clear();
    m_allReduceBuffer.clear();
    m_allReduceReceivedBytes.clear();
    m_allReduceLegacyMulticastDone.clear();
    m_portRankMap.clear();
    m_rankPortMap.clear();
    m_allGatherBuffer.clear();
    m_allGatherReceivedBytes.clear();
    m_seenUploadSeqs.clear();
    Simulator::Cancel(m_arbitrationEvent);
    NetDevice::DoDispose();
}

uint32_t
NvSwitch::AddPort(Ptr<NetDevice> device)
{
    NS_LOG_FUNCTION(this << device);
    uint32_t portNum = m_ports.size();
    m_ports.push_back(device);
    m_voqs.push_back(std::queue<VoqEntry>());
    m_portDegradationModels.push_back(nullptr);
    m_outputBusyUntil.push_back(Seconds(0));

    // Set up promiscuous receive callback
    device->SetPromiscReceiveCallback(
        MakeCallback(&NvSwitch::ReceiveFromPort, this));

    NS_LOG_INFO("Added port " << portNum << " to switch");
    return portNum;
}

uint32_t
NvSwitch::GetNPorts() const
{
    return m_ports.size();
}

Ptr<NetDevice>
NvSwitch::GetPort(uint32_t index) const
{
    if (index < m_ports.size())
    {
        return m_ports[index];
    }
    return nullptr;
}

std::string
NvSwitch::GetVendorName() const
{
    return "NVIDIA";
}

void
NvSwitch::SetVoqDepth(uint32_t depth)
{
    m_voqDepth = depth;
}

void
NvSwitch::SetArbitrationInterval(uint64_t intervalNs)
{
    m_arbitrationIntervalNs = intervalNs;
}

void
NvSwitch::SetArbiter(Ptr<Arbiter> arbiter)
{
    NS_LOG_FUNCTION(this << arbiter);
    if (arbiter)
    {
        m_arbiter = arbiter;
    }
}

Ptr<Arbiter>
NvSwitch::GetArbiter() const
{
    return m_arbiter;
}

void
NvSwitch::SetCutThroughDelay(uint64_t delayNs)
{
    m_cutThroughDelayNs = delayNs;
}

void
NvSwitch::SetAllReduceEnabled(bool enable)
{
    m_allReduceEnabled = enable;
}

void
NvSwitch::SetAllReduceThreshold(uint32_t threshold)
{
    m_allReduceThreshold = threshold;
}

void
NvSwitch::SetAllReduceAggregationDelay(uint64_t delayNs)
{
    m_allReduceAggregationDelayNs = delayNs;
}

void
NvSwitch::SetAllReduceDataSize(uint64_t size)
{
    m_allReduceDataSize = size;
}

void
NvSwitch::SetAllReduceNumPartitions(uint32_t numPartitions)
{
    m_allReduceNumPartitions = numPartitions;
}

void
NvSwitch::SetAllGatherEnabled(bool enable)
{
    m_allGatherEnabled = enable;
}

void
NvSwitch::SetAllGatherThreshold(uint32_t threshold)
{
    m_allGatherThreshold = threshold;
}

void
NvSwitch::SetAllGatherChunkSize(uint64_t size)
{
    m_allGatherChunkSize = size;
}

void
NvSwitch::SetAllGatherDataSize(uint64_t size)
{
    m_allGatherDataSize = size;
}

void
NvSwitch::SetPortDegradationModel(uint32_t port, Ptr<LinkDegradationModel> model)
{
    if (port >= m_ports.size())
    {
        NS_LOG_ERROR("Invalid port " << port);
        return;
    }
    if (port >= m_portDegradationModels.size())
    {
        m_portDegradationModels.resize(m_ports.size());
    }
    m_portDegradationModels[port] = model;
}

Ptr<LinkDegradationModel>
NvSwitch::GetPortDegradationModel(uint32_t port) const
{
    if (port < m_portDegradationModels.size())
    {
        return m_portDegradationModels[port];
    }
    return nullptr;
}

void
NvSwitch::SetFecModel(Ptr<FecModel> model)
{
    m_fecModel = model;
}

void
NvSwitch::SetFecOpticalOnly(bool opticalOnly)
{
    m_fecOpticalOnly = opticalOnly;
}

Ptr<FecModel>
NvSwitch::GetFecModel() const
{
    return m_fecModel;
}

bool
NvSwitch::PortUsesFec(uint32_t port) const
{
    if (!m_fecModel || !m_fecModel->IsEnabled())
    {
        return false;
    }
    if (!m_fecOpticalOnly)
    {
        return true;
    }
    if (port >= m_portDegradationModels.size() || !m_portDegradationModels[port])
    {
        return false;
    }
    return m_portDegradationModels[port]->GetLinkMetadata().medium == "optical";
}

void
NvSwitch::SetLlrEnabled(bool enabled)
{
    m_llrEnabled = enabled;
}

void
NvSwitch::AddStaticRoute(Mac48Address addr, uint32_t port)
{
    NS_LOG_FUNCTION(this << addr << port);
    m_macTable[addr] = port;
}

void
NvSwitch::AddRankPort(uint16_t rank, uint32_t port)
{
    NS_LOG_FUNCTION(this << rank << port);
    auto& ports = m_rankPortMap[rank];
    if (std::find(ports.begin(), ports.end(), port) == ports.end())
    {
        ports.push_back(port);
    }
}

std::unordered_map<uint16_t, std::vector<uint32_t>>
NvSwitch::BuildRankPorts() const
{
    // The learned map is authoritative for ranks that uploaded: a GPU's ports
    // may include links added after registration, and upload ingress is the
    // ground truth for which port actually reaches it. Registered ports fill
    // the gaps (and are appended, deduped, for spraying).
    std::unordered_map<uint16_t, std::vector<uint32_t>> rankPorts;
    for (const auto& entry : m_portRankMap)
    {
        rankPorts[entry.second].push_back(entry.first);
    }
    for (const auto& entry : m_rankPortMap)
    {
        auto& ports = rankPorts[entry.first];
        for (uint32_t port : entry.second)
        {
            if (std::find(ports.begin(), ports.end(), port) == ports.end())
            {
                ports.push_back(port);
            }
        }
    }
    return rankPorts;
}

void
NvSwitch::AddSourceBasedRoute(Mac48Address dstMac, uint16_t srcRank, uint32_t port)
{
    NS_LOG_FUNCTION(this << dstMac << srcRank << port);
    m_sourceBasedTable[{dstMac, srcRank}] = port;
}

void
NvSwitch::SetSourceBasedRouting(bool enable)
{
    NS_LOG_FUNCTION(this << enable);
    m_sourceBasedRoutingEnabled = enable;
}

void
NvSwitch::AddSprayPorts(Mac48Address dstMac, const std::vector<uint32_t>& ports)
{
    NS_LOG_FUNCTION(this << dstMac << ports.size());
    m_sprayPortsTable[dstMac] = ports;
    m_sprayRoundRobin[dstMac] = 0;  // Initialize round-robin counter
}

void
NvSwitch::SetSprayRouting(bool enable)
{
    NS_LOG_FUNCTION(this << enable);
    m_sprayRoutingEnabled = enable;
}

void
NvSwitch::ClearRoutingTables()
{
    m_macTable.clear();
    m_sourceBasedTable.clear();
    m_sprayPortsTable.clear();
    m_sprayRoundRobin.clear();
}

void
NvSwitch::SetFailureAwareRouting(bool enable)
{
    m_failureAwareRoutingEnabled = enable;
}

bool
NvSwitch::IsPortOperational(uint32_t port) const
{
    if (port >= m_ports.size() || !m_ports[port] || !m_ports[port]->IsLinkUp())
    {
        return false;
    }
    return port >= m_portDegradationModels.size()
           || !m_portDegradationModels[port]
           || m_portDegradationModels[port]->IsLinkUp();
}

bool
NvSwitch::RegisterUploadSeq(uint16_t flowId, uint16_t srcRank, uint32_t seqNum)
{
    auto& seen = m_seenUploadSeqs[flowId][srcRank];
    if (!seen.insert(seqNum).second)
    {
        m_sharpDuplicateUploads++;
        NS_LOG_DEBUG("Duplicate upload ignored: flowId=" << flowId
                     << " srcRank=" << srcRank << " seq=" << seqNum);
        return false;
    }
    return true;
}

void
NvSwitch::InjectAggregateUpload(uint16_t flowId, uint16_t srcRank, uint32_t seqNum,
                                uint32_t effectiveSize, bool allGather)
{
    FabricPacketType type = allGather ? FabricPacketType::ALLGATHER
                                      : FabricPacketType::ALLREDUCE;
    FabricHeader fh;
    fh.SetPacketType(type);
    fh.SetFlowId(flowId);
    fh.SetSourceRank(srcRank);
    fh.SetDestRank(0xFFFF);
    fh.SetSequenceNumber(seqNum);
    fh.SetPayloadSize(effectiveSize);
    fh.SetEffectiveDataSize(effectiveSize);
    Ptr<Packet> pkt = Create<Packet>(effectiveSize);
    pkt->AddHeader(fh);
    if (allGather)
    {
        ProcessAllGather(pkt, fh, 0);
    }
    else
    {
        ProcessAllReduce(pkt, fh, 0);
    }
}

bool
NvSwitch::InjectResultNack(uint16_t requestingRank, uint32_t seqNum)
{
    FabricHeader nh;
    nh.SetPacketType(FabricPacketType::NACK);
    nh.SetFlowId(1);
    nh.SetSourceRank(requestingRank);
    nh.SetDestRank(0xFFFF);
    nh.SetSequenceNumber(seqNum);
    return HandleSwitchNack(nh, 0);
}

size_t
NvSwitch::GetSharpResultStoreSize(uint16_t destRank) const
{
    // Sum every flow's window for the dest (concurrent flows keep
    // independent windows — see StoreMulticastResult's retention note).
    size_t total = 0;
    for (const auto& kv : m_sharpResultStore)
    {
        if ((kv.first >> 16) == destRank)
        {
            total += kv.second.size();
        }
    }
    return total;
}

bool
NvSwitch::ReceiveFromPort(Ptr<NetDevice> device, Ptr<const Packet> packet,
                           uint16_t protocol, const Address& source,
                           const Address& destination, NetDevice::PacketType packetType)
{
    NS_LOG_FUNCTION(this << device << packet << protocol << source << destination);

    // Copy packet since we need to modify it
    Ptr<Packet> pkt = packet->Copy();

    // Find the input port
    uint32_t inPort = 0;
    bool found = false;
    for (size_t i = 0; i < m_ports.size(); ++i)
    {
        if (m_ports[i] == device)
        {
            inPort = i;
            found = true;
            break;
        }
    }

    if (!found)
    {
        NS_LOG_ERROR("Received packet from unknown device");
        m_dropTrace(pkt);
        m_droppedPackets++;
        m_unknownPortDrops++;
        return false;
    }
    m_rxTrace(pkt);
    m_rxPackets++;
    m_rxBytes += pkt->GetSize();

    // Peek FabricHeader before BER/FEC check so we can send NACK back
    // on failure. In our probability-based BER model, the header is always
    // readable (BER is an abstract coin flip, not actual bit corruption).
    FabricHeader fabricHeader;
    bool hasFabricHeader = pkt->PeekHeader(fabricHeader);

    // Apply link degradation model if present for this port.
    // Protocol control and retry packets (CREDIT, ACK, NACK, RETRY_REQUEST,
    // RETRY_ACK, PERMANENT_LOSS, CNP) are reliable against BER corruption —
    // matching real NVLink behavior where control traffic bypasses CRC/FEC
    // checks. CNP must bypass BER so DCQCN feedback cannot be corrupted under
    // BER>0 (a corrupted CNP would stall the rate machine). The
    // IsControlPacket()/IsRetryPacket() predicates are the single source of
    // truth — an inline list here previously omitted CNP.
    // Only data/collective/p2p packets go through BER sampling.
    bool isControlPacket = false;
    if (hasFabricHeader)
    {
        isControlPacket = fabricHeader.IsControlPacket() || fabricHeader.IsRetryPacket();
    }

    if (inPort < m_portDegradationModels.size() && m_portDegradationModels[inPort]
        && !isControlPacket)
    {
        Ptr<LinkDegradationModel> linkModel = m_portDegradationModels[inPort];
        bool failed = false;

        if (!linkModel->IsLinkUp())
        {
            failed = true;
        }
        else if (linkModel->GetPacketLossRate() > 0.0
                 && linkModel->GetRandomValue() < linkModel->GetPacketLossRate())
        {
            failed = true;
        }
        else if (PortUsesFec(inPort))
        {
            uint32_t payloadSize = hasFabricHeader ? fabricHeader.GetPayloadSize() : pkt->GetSize();
            failed = m_fecModel->DecodePacket(linkModel->GetBer(), payloadSize)
                     == FecResult::UNCORRECTABLE;
        }
        else
        {
            // ProcessPacketBerOnly (not ProcessPacket): the loss-rate draw
            // already happened above — ProcessPacket would draw the loss
            // rate a second time, compounding the effective loss.
            failed = !linkModel->ProcessPacketBerOnly(pkt);
        }

        if (failed)
        {
            if (hasFabricHeader && m_llrEnabled && linkModel->IsLinkUp())
            {
                FabricHeader nackHeader;
                nackHeader.SetPacketType(FabricPacketType::NACK);
                nackHeader.SetFabricType(fabricHeader.GetFabricType());
                nackHeader.SetSourceRank(fabricHeader.GetDestRank());
                nackHeader.SetDestRank(fabricHeader.GetSourceRank());
                nackHeader.SetSequenceNumber(fabricHeader.GetSequenceNumber());
                nackHeader.SetFlowId(fabricHeader.GetFlowId());
                nackHeader.SetVirtualChannel(fabricHeader.GetVirtualChannel());
                nackHeader.SetSourceMac(fabricHeader.GetDestMac());
                nackHeader.SetDestMac(fabricHeader.GetSourceMac());

                Ptr<Packet> nackPkt = Create<Packet>(0);
                nackPkt->AddHeader(nackHeader);
                m_ports[inPort]->Send(nackPkt, Mac48Address::ConvertFrom(source), 0x0800);
            }
            m_dropTrace(pkt);
            m_droppedPackets++;
            m_linkErrorDrops++;
            return true;
        }
    }

    // Extract source MAC from L2 address (the directly connected device)
    Mac48Address l2SrcAddr = Mac48Address::ConvertFrom(source);
    Mac48Address l2DstAddr = Mac48Address::ConvertFrom(destination);

    // Check for FabricHeader - peek to get actual source/destination for routing
    // (already peeked above before BER/FEC check)
    Mac48Address srcAddr = l2SrcAddr;
    Mac48Address dstAddr = l2DstAddr;

    if (hasFabricHeader)
    {
        // Decrement TTL for multi-hop forwarding
        uint8_t ttl = fabricHeader.GetTtl();
        if (ttl <= 1)
        {
            NS_LOG_DEBUG("Packet TTL expired, dropping");
            m_dropTrace(pkt);
            m_droppedPackets++;
            m_ttlDrops++;
            return true;
        }
        // Remove and re-add header with decremented TTL
        pkt->RemoveHeader(fabricHeader);
        fabricHeader.SetTtl(ttl - 1);
        pkt->AddHeader(fabricHeader);

        // Use FabricHeader's MAC addresses for routing (end-to-end GPU addresses)
        srcAddr = fabricHeader.GetSourceMac();
        dstAddr = fabricHeader.GetDestMac();

        // Handle All-Reduce packets
        if (m_allReduceEnabled && fabricHeader.GetPacketType() == FabricPacketType::ALLREDUCE)
        {
            ProcessAllReduce(pkt, fabricHeader, inPort);
            return true;
        }

        // Handle All-Gather packets (NVLS)
        if (m_allGatherEnabled && fabricHeader.GetPacketType() == FabricPacketType::ALLGATHER)
        {
            ProcessAllGather(pkt, fabricHeader, inPort);
            return true;
        }

        // NACK addressed to the switch sentinel rank (0xFFFF): the receiver
        // lost a multicast result packet and is asking THIS switch to retry.
        // Must be intercepted before routing — its DestRank is the sentinel,
        // not a routable GPU, and flooding it to every port would only feed
        // every endpoint a NACK whose seq matches nothing (retry misses).
        if (fabricHeader.GetPacketType() == FabricPacketType::NACK &&
            fabricHeader.GetDestRank() == 0xFFFF)
        {
            HandleSwitchNack(fabricHeader, inPort);
            return true;
        }
    }

    // Learn source MAC address only if not already in routing table
    // This prevents overwriting static routes with learned routes
    if (m_macTable.find(srcAddr) == m_macTable.end())
    {
        LearnMacAddress(srcAddr, inPort);
    }
    int32_t outPort = -1;

    if (m_sprayRoutingEnabled)
    {
        // Use spray routing: distribute packets round-robin across destination's ports
        auto portsIt = m_sprayPortsTable.find(dstAddr);
        if (portsIt != m_sprayPortsTable.end() && !portsIt->second.empty())
        {
            auto& ports = portsIt->second;
            uint32_t& rrIndex = m_sprayRoundRobin[dstAddr];
            for (uint32_t attempt = 0; attempt < ports.size(); ++attempt)
            {
                const uint32_t index = (rrIndex + attempt) % ports.size();
                const uint32_t candidate = ports[index];
                if (candidate != inPort && IsPortOperational(candidate))
                {
                    outPort = candidate;
                    rrIndex = (index + 1) % ports.size();
                    break;
                }
            }
        }
    }

    if (outPort < 0 && m_sourceBasedRoutingEnabled && hasFabricHeader)
    {
        // Use source-based routing: route based on (destination MAC, source rank)
        uint16_t srcRank = fabricHeader.GetSourceRank();
        auto it = m_sourceBasedTable.find({dstAddr, srcRank});
        if (it != m_sourceBasedTable.end())
        {
            if (it->second != inPort && IsPortOperational(it->second))
            {
                outPort = it->second;
            }
        }
    }

    if (outPort < 0)
    {
        // Fall back to destination-only routing
        const int32_t staticPort = LookupOutputPort(dstAddr);
        if (staticPort >= 0 && static_cast<uint32_t>(staticPort) != inPort
            && IsPortOperational(static_cast<uint32_t>(staticPort)))
        {
            outPort = staticPort;
        }
    }

    if (outPort < 0)
    {
        if (m_failureAwareRoutingEnabled)
        {
            NS_LOG_DEBUG("No surviving route from port " << inPort << " dst=" << dstAddr);
            m_dropTrace(pkt);
            m_droppedPackets++;
            m_routeUnavailableDrops++;
            return true;
        }

        // Unknown destination - flood to all operational ports except input
        NS_LOG_DEBUG("Flooding packet from port " << inPort << " dst=" << dstAddr);
        for (size_t i = 0; i < m_ports.size(); ++i)
        {
            if (i != inPort && IsPortOperational(i))
            {
                VoqEntry entry;
                entry.packet = pkt->Copy();
                entry.srcAddr = srcAddr;
                entry.dstAddr = dstAddr;
                entry.inPort = inPort;
                EnqueueToVoq(i, entry);
            }
        }
    }
    else if (static_cast<uint32_t>(outPort) != inPort)
    {
        // Forward to specific output port
        VoqEntry entry;
        entry.packet = pkt;
        entry.srcAddr = srcAddr;
        entry.dstAddr = dstAddr;
        entry.inPort = inPort;
        EnqueueToVoq(outPort, entry);
    }
    else
    {
        // Packet arrived on correct port, drop
        NS_LOG_DEBUG("Packet arrived on correct port " << inPort << " dst=" << dstAddr);
    }

    // Schedule arbitration if not already scheduled
    if (!m_arbitrationEvent.IsPending())
    {
        ScheduleArbitration();
    }

    return true;
}

void
NvSwitch::LearnMacAddress(Mac48Address addr, uint32_t port)
{
    NS_LOG_FUNCTION(this << addr << port);
    m_macTable[addr] = port;
}

int32_t
NvSwitch::LookupOutputPort(Mac48Address addr)
{
    auto it = m_macTable.find(addr);
    if (it != m_macTable.end())
    {
        return it->second;
    }
    return -1;
}

void
NvSwitch::EnqueueToVoq(uint32_t outPort, const VoqEntry& entry)
{
    NS_LOG_FUNCTION(this << outPort);

    if (outPort >= m_voqs.size())
    {
        NS_FATAL_ERROR("Invalid output port " << outPort << " >= " << m_voqs.size());
    }

    if (!IsPortOperational(outPort))
    {
        m_dropTrace(entry.packet);
        m_droppedPackets++;
        m_routeUnavailableDrops++;
        return;
    }

    if (m_voqs[outPort].size() >= m_voqDepth)
    {
        NS_LOG_WARN("VOQ for port " << outPort << " is full, dropping packet");
        m_dropTrace(entry.packet);
        m_droppedPackets++;
        m_voqDrops++;
        return;
    }

    NS_ASSERT_MSG(m_voqs[outPort].size() < m_voqDepth,
                  "NvSwitch::EnqueueToVoq: VOQ overflow for port " << outPort
                  << ", size=" << m_voqs[outPort].size()
                  << " depth=" << m_voqDepth);

    // Congestion hook: let a subclass (EthernetSwitch) mark the SoHeader ECN
    // bit on enqueue (RoCE lossless) or tail-drop with a NACK (plain Ethernet
    // lossy), before the fast-path / push. The depth here is the queue the
    // packet is arriving into (excluding itself); the empty-VOQ fast-path below
    // sees depth 0 and so produces no mark/drop -- matching RoCE marking
    // semantics where only packets arriving behind a backlog get marked.
    // A false return drops the packet (caller counts a VOQ drop); true admits.
    if (!OnPacketEnqueue(entry.packet, outPort, entry.inPort,
                         m_voqs[outPort].size()))
    {
        m_dropTrace(entry.packet);
        m_droppedPackets++;
        m_voqDrops++;
        return;
    }

    // For non-blocking switch with egress serialization:
    // If VOQ is empty and output port is free, forward immediately with
    // serialization delay. Otherwise enqueue and wait for arbitration.
    if (m_voqs[outPort].empty() && Simulator::Now() >= m_outputBusyUntil[outPort])
    {
        ForwardPacket(entry.packet, entry.srcAddr, entry.dstAddr, outPort);
        return;
    }

    m_voqs[outPort].push(entry);
}

bool
NvSwitch::OnPacketEnqueue(Ptr<Packet> packet, uint32_t outPort,
                          uint32_t inPort, size_t voqDepth)
{
    // Default no-op: NVSwitch (scale-up fabric) does no ECN marking or lossy
    // drop, so Su parity is unaffected. EthernetSwitch overrides to mark the
    // SoHeader (RoCE) or tail-drop with a NACK (plain Ethernet). Returning
    // true admits the packet (the caller pushes it to the VOQ).
    (void)packet;
    (void)outPort;
    (void)inPort;
    (void)voqDepth;
    return true;
}

void
NvSwitch::EmitNackBack(const FabricHeader& orig, uint32_t returnPort,
                       Mac48Address destMac)
{
    // No retransmission machinery active -> a NACK would be unprocessable.
    // Mirrors the BER-NACK gate in ReceiveFromPort (m_llrEnabled).
    if (!m_llrEnabled)
    {
        return;
    }
    if (returnPort >= m_ports.size())
    {
        return;
    }
    FabricHeader nackHeader;
    nackHeader.SetPacketType(FabricPacketType::NACK);
    nackHeader.SetFabricType(orig.GetFabricType());
    nackHeader.SetSourceRank(orig.GetDestRank());
    nackHeader.SetDestRank(orig.GetSourceRank());
    nackHeader.SetSequenceNumber(orig.GetSequenceNumber());
    nackHeader.SetFlowId(orig.GetFlowId());
    nackHeader.SetVirtualChannel(orig.GetVirtualChannel());
    nackHeader.SetSourceMac(orig.GetDestMac());
    nackHeader.SetDestMac(orig.GetSourceMac());

    Ptr<Packet> nackPkt = Create<Packet>(0);
    nackPkt->AddHeader(nackHeader);
    m_ports[returnPort]->Send(nackPkt, destMac, 0x0800);
}

void
NvSwitch::Arbitrate()
{
    NS_LOG_FUNCTION(this);

    // Ask the arbitration strategy which output ports to drain this cycle.
    // The strategy decides grants; the switch drains the VOQ front packet
    // onto the wire (egress serialization) — forwarding + rescheduling stay
    // here. The default RoundRobinArbiter reproduces the historical
    // non-blocking crossbar (one packet per free, non-empty output port).
    if (m_arbiter)
    {
        std::vector<ArbiterGrant> grants =
            m_arbiter->SelectGrants(m_voqs, m_outputBusyUntil, Simulator::Now());
        for (const ArbiterGrant& g : grants)
        {
            if (g.port >= m_voqs.size() || m_voqs[g.port].empty())
            {
                continue;
            }
            VoqEntry entry = m_voqs[g.port].front();
            m_voqs[g.port].pop();
            ForwardPacket(entry.packet, entry.srcAddr, entry.dstAddr, g.port);
        }
    }

    // Schedule next arbitration at earliest port-free time with queued packets
    bool hasQueuedPackets = false;
    Time earliestFree = Time::Max();

    for (uint32_t port = 0; port < m_voqs.size(); ++port)
    {
        if (!m_voqs[port].empty())
        {
            hasQueuedPackets = true;
            earliestFree = std::min(earliestFree, m_outputBusyUntil[port]);
        }
    }

    if (hasQueuedPackets)
    {
        Time delay = earliestFree - Simulator::Now();
        if (delay <= Seconds(0))
        {
            // Some ports are free now with queued packets
            m_arbitrationEvent = Simulator::Schedule(NanoSeconds(1), &NvSwitch::Arbitrate, this);
        }
        else
        {
            // All ports with queued packets are busy; schedule when earliest becomes free
            m_arbitrationEvent = Simulator::Schedule(delay, &NvSwitch::Arbitrate, this);
        }
    }
}

void
NvSwitch::ScheduleArbitration()
{
    NS_LOG_FUNCTION(this);
    m_arbitrationEvent = Simulator::Schedule(NanoSeconds(m_arbitrationIntervalNs),
                                              &NvSwitch::Arbitrate, this);
}

void
NvSwitch::ForwardPacket(Ptr<Packet> packet, Mac48Address srcAddr,
                         Mac48Address dstAddr, uint32_t outPort)
{
    NS_LOG_FUNCTION(this << packet << srcAddr << dstAddr << outPort);

    NS_ASSERT_MSG(outPort < m_ports.size(),
                  "ForwardPacket: invalid output port " << outPort << " >= " << m_ports.size());
    if (outPort >= m_ports.size())
    {
        return;
    }

    if (!IsPortOperational(outPort))
    {
        m_dropTrace(packet);
        m_droppedPackets++;
        m_routeUnavailableDrops++;
        return;
    }

    Ptr<NetDevice> outDevice = m_ports[outPort];
    uint64_t wireSize = packet->GetSize();
    Time codingLatency = Seconds(0);
    if (m_fecOpticalOnly && PortUsesFec(outPort))
    {
        FabricHeader header;
        if (packet->PeekHeader(header))
        {
            wireSize = header.GetSerializedSize()
                       + m_fecModel->GetEncodedSize(header.GetPayloadSize());
        }
        codingLatency = m_fecModel->GetEncodeLatency() + m_fecModel->GetDecodeLatency();
    }
    m_txTrace(packet);
    m_txPackets++;
    m_txBytes += wireSize;

    // Compute egress serialization time: packet must serialize on the output
    // link at the link's data rate. This models per-output-port contention —
    // packets targeting the same port must serialize one after another.
    Time serializationTime = Seconds(0);
    Ptr<PointToPointNetDevice> p2pOut = DynamicCast<PointToPointNetDevice>(outDevice);
    if (p2pOut)
    {
        DataRateValue rateValue;
        p2pOut->GetAttribute("DataRate", rateValue);
        DataRate outputRate = rateValue.Get();
        // packet size in bytes * 8 bits/byte / output rate in bits/s
        serializationTime = Seconds(static_cast<double>(wireSize * 8) / outputRate.GetBitRate());
    }

    // Update port busy time: port is occupied during serialization
    Time startTime = std::max(Simulator::Now(), m_outputBusyUntil[outPort]);
    m_outputBusyUntil[outPort] = startTime + NanoSeconds(m_cutThroughDelayNs) + serializationTime;

    // Egress serialization + cut-through pipeline + propagation delay
    // models the full packet traversal including per-port contention.
    // Cut-through: header starts after pipeline delay, tail finishes after
    // serialization. Completion time (for credit/dependency) uses full duration.
    // The scheduled path must also run for FEC-active egress when cut-through
    // is disabled: outDevice->Send() serializes the RAW packet, discarding the
    // FEC wire expansion (wireSize) and codec latency computed above. With
    // m_cutThroughDelayNs == 0 the pipeline term is zero, so the scheduled
    // arrival is startTime + serialization(encoded) + codec + propagation —
    // store-and-forward timing with FEC applied. Non-FEC ports keep the
    // Send() fallback below unchanged.
    const bool fecActiveEgress = p2pOut && m_fecOpticalOnly && PortUsesFec(outPort);
    if (p2pOut && (m_cutThroughDelayNs > 0 || fecActiveEgress))
    {
        Ptr<Channel> rawChannel = p2pOut->GetChannel();
        Ptr<PointToPointChannel> channel = DynamicCast<PointToPointChannel>(rawChannel);
        if (channel)
        {
            Ptr<PointToPointNetDevice> dstDevice;
            if (channel->GetPointToPointDevice(0) == p2pOut)
            {
                dstDevice = channel->GetPointToPointDevice(1);
            }
            else
            {
                dstDevice = channel->GetPointToPointDevice(0);
            }

            TimeValue delayValue;
            channel->GetAttribute("Delay", delayValue);
            Time propDelay = delayValue.Get();

            // Schedule Receive() at completion time:
            // startTime + pipeline + serialization + propagation
            Time totalDelay = (startTime - Simulator::Now())
                            + NanoSeconds(m_cutThroughDelayNs)
                            + serializationTime
                            + codingLatency
                            + propDelay;

            PppHeader ppp;
            ppp.SetProtocol(0x0021);
            Ptr<Packet> pppPacket = packet->Copy();
            pppPacket->AddHeader(ppp);

            Simulator::ScheduleWithContext(dstDevice->GetNode()->GetId(),
                                           totalDelay,
                                           &PointToPointNetDevice::Receive,
                                           dstDevice,
                                           pppPacket);
            return;
        }
    }

    // Fallback: store-and-forward for non-PointToPoint or when cut-through is disabled
    bool result = outDevice->Send(packet, Mac48Address::GetBroadcast(), 0x0800);
    if (!result)
    {
        NS_LOG_ERROR("Send failed on port " << outPort
                     << " packet size " << packet->GetSize());
    }
}

void
NvSwitch::ProcessAllReduce(Ptr<Packet> packet, const FabricHeader& header,
                            uint32_t inPort)
{
    NS_LOG_FUNCTION(this << packet << inPort);

    uint16_t flowId = header.GetFlowId();
    uint16_t srcRank = header.GetSourceRank();

    // Dedup gate: a retransmitted upload (its LLR ack lost) must not count
    // as a new contribution. Still re-ACK so the sender purges its retry
    // entry, but the aggregation ledgers below advance only once per seq.
    if (!RegisterUploadSeq(flowId, srcRank, header.GetSequenceNumber()))
    {
        SendSharpLlrAck(header, inPort);
        return;
    }

    // Learn port-to-rank mapping for multicast
    m_portRankMap[inPort] = srcRank;

    // Track bytes received per (flowId, srcRank). Effective bytes only: the
    // collective spray path stamps each fragment's share of the effective
    // payload (fabric-endpoint-send.cc), and a padding fragment legitimately
    // carries ZERO effective bytes while holding real wire bytes. The old
    // zero → payloadSize fallback credited such fragments with their full
    // wire payload, so a rank's ledger hit its threshold from padding alone
    // and the switch aggregated before the rank's real data arrived (codex
    // round 19, finding 3: 4-GPU/1-byte AllReduce, rank 0's effective-byte
    // lane delayed 100 µs — completed at 2 µs with rank 0 at 0/1 bytes).
    // Every upload producer stamps the field; zero means zero.
    uint32_t effectiveSize = header.GetEffectiveDataSize();
    m_allReduceReceivedBytes[flowId][srcRank] += effectiveSize;

    // LLR ack the accepted upload so the sender purges its retry entry
    // (uploads ARE stored endpoint-side now; without an ack every entry
    // would live until the retry limit declared a false permanent loss).
    SendSharpLlrAck(header, inPort);

    // Store chunk sizes only (not full Packet objects) to avoid memory explosion.
    // Data content is dummy; only sizes are needed for multicast result creation.
    m_allReduceBuffer[flowId][srcRank].push_back(
        {header.GetPayloadSize(), effectiveSize});

    // Count how many unique GPUs have contributed to this flow
    uint32_t sourceCount = m_allReduceReceivedBytes[flowId].size();

    NS_LOG_DEBUG("SHARP AllReduce: flowId=" << flowId << " srcRank=" << srcRank
                 << " sources=" << sourceCount << "/" << m_allReduceThreshold
                 << " bytes=" << m_allReduceReceivedBytes[flowId][srcRank]);

    // Pipelined multicast: partition dataSize into numPartitions and multicast
    // each partition as soon as all GPUs have sent enough cumulative bytes.
    // This models real NVLS where the switch reduces data element-by-element
    // as it arrives and starts multicasting reduced partitions immediately.
    // 0 means SINGLE-MULTICAST (legacy) mode, per the header contract
    // ("0=old single-multicast mode, >=2=pipelined") — substituting the
    // threshold here made 0 behave as N partitions, so --nvlsNumPartitions=0
    // and =N produced identical pipelined runs (34.5 us / 48 switch
    // transmissions at 4 GPU / 1 MiB / 2 links) while the sim's help text
    // promised "0=single" (codex round 22, finding 7). 1 is also single
    // (one partition covers the whole buffer), >= 2 pipelined.
    uint32_t numPartitions = m_allReduceNumPartitions;

    if (numPartitions >= 2 && m_allReduceDataSize > 0 && sourceCount >= m_allReduceThreshold)
    {
        // Pipelined mode: check for newly completed partitions
        uint64_t partitionSize = m_allReduceDataSize / numPartitions;
        // Handle remainder: last partition may be slightly larger
        uint64_t remainder = m_allReduceDataSize - partitionSize * (numPartitions - 1);

        uint32_t previousCompleted = m_allReduceCompletedPartitions[flowId];
        uint32_t currentCompleted = 0;

        for (uint32_t k = 0; k < numPartitions; k++)
        {
            uint64_t requiredBytes = (k < numPartitions - 1)
                ? (k + 1) * partitionSize
                : m_allReduceDataSize; // last partition requires full dataSize

            bool partitionComplete = true;
            for (const auto& src : m_allReduceReceivedBytes[flowId])
            {
                if (src.second < requiredBytes)
                {
                    partitionComplete = false;
                    break;
                }
            }
            if (partitionComplete)
            {
                currentCompleted = k + 1;
            }
            else
            {
                break; // partitions must complete in order
            }
        }

        // Multicast newly completed partitions
        for (uint32_t k = previousCompleted; k < currentCompleted; k++)
        {
            uint64_t thisPartitionSize = (k < numPartitions - 1) ? partitionSize : remainder;
            const auto& firstSourceChunks =
                m_allReduceBuffer[flowId].begin()->second;
            uint64_t receivedWireBytes = 0;
            uint64_t receivedEffectiveBytes = 0;
            for (const auto& chunk : firstSourceChunks)
            {
                receivedWireBytes += chunk.wireBytes;
                receivedEffectiveBytes += chunk.effectiveBytes;
            }
            // Padding fragments carry wire bytes but zero effective bytes
            // (codex round 19, finding 3): a source whose accepted uploads
            // are all padding sums to receivedEffectiveBytes == 0, and the
            // wire/effective ratio below would divide by zero. That cannot
            // happen when the partition check runs on effective ledgers
            // (every source has >= requiredBytes effective before we get
            // here), but guard anyway — a zero-effective source maps its
            // partition to wire bytes at the ratio 1 (no expansion).
            uint64_t ratioWhole;
            uint64_t ratioRemainder;
            uint64_t wirePartitionSize;
            if (receivedEffectiveBytes == 0)
            {
                ratioWhole = 1;
                ratioRemainder = 0;
                wirePartitionSize = thisPartitionSize;
            }
            else
            {
                ratioWhole = receivedWireBytes / receivedEffectiveBytes;
                ratioRemainder = receivedWireBytes % receivedEffectiveBytes;
                wirePartitionSize =
                    thisPartitionSize * ratioWhole +
                    (thisPartitionSize * ratioRemainder +
                     receivedEffectiveBytes - 1) /
                        receivedEffectiveBytes;
            }
            m_deferredResultEvents.push_back(
                Simulator::Schedule(NanoSeconds(m_allReduceAggregationDelayNs),
                                    &NvSwitch::MulticastAllReducePartition, this,
                                    flowId, k, thisPartitionSize, wirePartitionSize,
                                    numPartitions));
        }

        m_allReduceCompletedPartitions[flowId] = currentCompleted;
    }
    else if (sourceCount >= m_allReduceThreshold)
    {
        // Legacy single-multicast mode (numPartitions <= 1 or dataSize not set)
        // Check if all GPUs have contributed their full data
        bool allComplete = false;
        if (m_allReduceDataSize > 0)
        {
            allComplete = true;
            for (const auto& src : m_allReduceReceivedBytes[flowId])
            {
                if (src.second < m_allReduceDataSize)
                {
                    allComplete = false;
                    break;
                }
            }
        }
        else
        {
            allComplete = true;
        }

        if (allComplete
            && m_allReduceLegacyMulticastDone.find(flowId)
                   == m_allReduceLegacyMulticastDone.end())
        {
            // Mark BEFORE scheduling (codex round 22, finding 1): the
            // allComplete test re-runs on every later upload — late padding
            // fragments that land after this multicast clears the buffer
            // would schedule a SECOND result event onto the cleared buffer
            // (empty begin() deref, SIGSEGV). The collective is one-shot;
            // once its result has gone out, later fragments are wire noise.
            m_allReduceLegacyMulticastDone.insert(flowId);
            m_deferredResultEvents.push_back(
                Simulator::Schedule(NanoSeconds(m_allReduceAggregationDelayNs),
                                    &NvSwitch::MulticastAllReduceResult, this,
                                    flowId, sourceCount));
        }
    }
}

void
NvSwitch::MulticastAllReduceResult(uint16_t flowId, uint32_t sourceCount)
{
    NS_LOG_FUNCTION(this << flowId << sourceCount);

    auto& gpuBuffers = m_allReduceBuffer[flowId];

    // Each GPU sent dataSize bytes. The switch reduces all N contributions
    // element-wise, producing a single result of dataSize. We use one GPU's
    // chunk sizes to create result packets of the same sizes. Copy the chunk
    // sizes BEFORE clearing the buffer: gpuBuffers.clear() destroys the map
    // values the reference would dangle into.
    auto firstGpu = gpuBuffers.begin();
    const auto chunkSizes = firstGpu->second;

    // Clear buffer before multicast
    gpuBuffers.clear();

    // Group ports by GPU rank for spraying chunks across multiple links per GPU
    std::unordered_map<uint16_t, std::vector<uint32_t>> rankPorts = BuildRankPorts();

    // Multicast result packets to each GPU. Create new packets from stored
    // chunk sizes (data content is dummy — only size matters for timing).
    for (auto& rankPortList : rankPorts)
    {
        uint16_t destRank = rankPortList.first;
        const auto& ports = rankPortList.second;

        for (size_t chunkIdx = 0; chunkIdx < chunkSizes.size(); ++chunkIdx)
        {
            uint32_t thisChunkSize = chunkSizes[chunkIdx].wireBytes;
            uint32_t thisEffectiveSize = chunkSizes[chunkIdx].effectiveBytes;

            FabricHeader resultHeader;
            resultHeader.SetPacketType(FabricPacketType::ALLREDUCE);
            resultHeader.SetFlowId(flowId);
            resultHeader.SetSourceRank(0xFFFF);  // sentinel: result from switch
            resultHeader.SetDestRank(destRank);
            resultHeader.SetPayloadSize(thisChunkSize);
            resultHeader.SetEffectiveDataSize(thisEffectiveSize);
            // Per-(dest,flow) seq: reorder buffers expect each
            // (vc,flow,source) stream to start at 0 (see m_multicastSeqNums'
            // note).
            resultHeader.SetSequenceNumber(
                m_multicastSeqNums[(uint64_t{destRank} << 16) | flowId]++);
            m_multicastSeqNum++;

            Ptr<Packet> pkt = Create<Packet>(thisChunkSize);
            pkt->AddHeader(resultHeader);

            // Spray chunks across this GPU's ports
            uint32_t portIdx = ports[chunkIdx % ports.size()];
            StoreMulticastResult(destRank, flowId, portIdx,
                                 resultHeader.GetSequenceNumber(), pkt);
            SendMulticastResultOnPort(portIdx, pkt);
        }
    }

    NS_LOG_INFO("SHARP AllReduce: multicast " << chunkSizes.size()
                << " result packets for flowId=" << flowId
                << " to " << rankPorts.size() << " GPUs");

    // Flow complete: retire the byte ledgers. The upload dedup ledger
    // (m_seenUploadSeqs) deliberately SURVIVES the result: a retransmitted
    // upload whose LLR ack raced the multicast must still be rejected, or
    // the replay re-accumulates the ledgers and the switch multicasts a
    // second full result from bytes it already holds (codex round 12,
    // finding 5). It is one uint32 per accepted upload, cleared in
    // DoDispose with the other per-flow state.
    m_allReduceReceivedBytes.erase(flowId);
}

void
NvSwitch::MulticastAllReducePartition(uint16_t flowId, uint32_t partitionIdx,
                                       uint64_t effectivePartitionSize,
                                       uint64_t wirePartitionSize,
                                       uint32_t numPartitions)
{
    NS_LOG_FUNCTION(this << flowId << partitionIdx << effectivePartitionSize
                         << wirePartitionSize << numPartitions);

    // Group ports by GPU rank for spraying chunks across multiple links per GPU
    std::unordered_map<uint16_t, std::vector<uint32_t>> rankPorts = BuildRankPorts();

    // Create result packets totaling wirePartitionSize, sprayed across GPU's ports.
    // Ensure at least one chunk per link (like SendCollective) so all links are
    // utilized and multicast uses aggregate BW, not single-link BW. Large
    // partitions use coalesced packet units, retaining bytes and path balance
    // while resolving queue events at a coarser granularity.
    const uint64_t sprayChunkSize = 131072; // 128KB
    const uint64_t maxChunkSize = 8ull * 1024 * 1024;
    const uint64_t coalesceThreshold = 512ull * 1024 * 1024;
    for (auto& rankPortList : rankPorts)
    {
        uint16_t destRank = rankPortList.first;
        const auto& ports = rankPortList.second;

        uint64_t minChunks = ports.size();
        uint64_t sizeBasedChunks =
            (wirePartitionSize + sprayChunkSize - 1) / sprayChunkSize;
        uint64_t numChunks = std::max(minChunks, sizeBasedChunks);
        uint64_t coalescedChunks =
            (wirePartitionSize + maxChunkSize - 1) / maxChunkSize;
        if (wirePartitionSize > coalesceThreshold && coalescedChunks > minChunks)
        {
            uint64_t pathBalancedChunks =
                ((coalescedChunks + minChunks - 1) / minChunks) * minChunks;
            numChunks = std::max(minChunks, std::min(sizeBasedChunks, pathBalancedChunks));
        }
        uint64_t chunkSize = (wirePartitionSize + numChunks - 1) / numChunks;

        uint64_t bytesRemaining = wirePartitionSize;
        for (uint64_t chunkIdx = 0; chunkIdx < numChunks; chunkIdx++)
        {
            uint32_t thisChunkSize = static_cast<uint32_t>(
                std::min(static_cast<uint64_t>(chunkSize), bytesRemaining));
            bytesRemaining -= thisChunkSize;
            uint64_t effectiveBegin =
                chunkIdx * effectivePartitionSize / numChunks;
            uint64_t effectiveEnd =
                (chunkIdx + 1) * effectivePartitionSize / numChunks;
            uint32_t thisEffectiveSize =
                static_cast<uint32_t>(effectiveEnd - effectiveBegin);

            Ptr<Packet> pkt = Create<Packet>(thisChunkSize);

            FabricHeader resultHeader;
            resultHeader.SetPacketType(FabricPacketType::ALLREDUCE);
            resultHeader.SetFlowId(flowId);
            resultHeader.SetSourceRank(0xFFFF);  // sentinel: result from switch
            resultHeader.SetDestRank(destRank);
            resultHeader.SetPayloadSize(thisChunkSize);
            resultHeader.SetEffectiveDataSize(thisEffectiveSize);
            // Per-(dest,flow) seq: reorder buffers expect each
            // (vc,flow,source) stream to start at 0 (see m_multicastSeqNums'
            // note).
            resultHeader.SetSequenceNumber(
                m_multicastSeqNums[(uint64_t{destRank} << 16) | flowId]++);
            m_multicastSeqNum++;

            pkt->AddHeader(resultHeader);

            uint32_t portIdx = ports[chunkIdx % ports.size()];
            StoreMulticastResult(destRank, flowId, portIdx,
                                 resultHeader.GetSequenceNumber(), pkt);
            SendMulticastResultOnPort(portIdx, pkt);
        }
    }

    // Clean up buffer after last partition is multicast. m_seenUploadSeqs
    // survives (see the non-partitioned path): late replays of the flow's
    // upload seqs must stay rejected after the result, not re-counted.
    if (partitionIdx == numPartitions - 1)
    {
        m_allReduceBuffer.erase(flowId);
        m_allReduceReceivedBytes.erase(flowId);
        m_allReduceCompletedPartitions.erase(flowId);
    }

    NS_LOG_INFO("SHARP AllReduce: multicast partition " << partitionIdx
                << "/" << numPartitions << " (" << effectivePartitionSize
                << " effective bytes, " << wirePartitionSize << " wire bytes)"
                << " for flowId=" << flowId
                << " to " << rankPorts.size() << " GPUs");
}

// NetDevice base class methods

void
NvSwitch::SetIfIndex(const uint32_t index)
{
    m_ifIndex = index;
}

uint32_t
NvSwitch::GetIfIndex() const
{
    return m_ifIndex;
}

Ptr<Channel>
NvSwitch::GetChannel() const
{
    return nullptr;
}

void
NvSwitch::SetAddress(Address address)
{
    m_address = Mac48Address::ConvertFrom(address);
}

Address
NvSwitch::GetAddress() const
{
    return m_address;
}

bool
NvSwitch::SetMtu(const uint16_t mtu)
{
    m_mtu = mtu;
    return true;
}

uint16_t
NvSwitch::GetMtu() const
{
    return m_mtu;
}

bool
NvSwitch::IsLinkUp() const
{
    return true;
}

void
NvSwitch::AddLinkChangeCallback(Callback<void> callback)
{
    // Not implemented for switch
}

bool
NvSwitch::IsBroadcast() const
{
    return true;
}

Address
NvSwitch::GetBroadcast() const
{
    return Mac48Address::GetBroadcast();
}

bool
NvSwitch::IsMulticast() const
{
    return true;
}

Address
NvSwitch::GetMulticast(Ipv4Address multicastGroup) const
{
    return Mac48Address::GetMulticast(multicastGroup);
}

Address
NvSwitch::GetMulticast(Ipv6Address address) const
{
    return Mac48Address::GetMulticast(address);
}

bool
NvSwitch::IsPointToPoint() const
{
    return false;
}

bool
NvSwitch::IsBridge() const
{
    return true;
}

bool
NvSwitch::Send(Ptr<Packet> packet, const Address& dest, uint16_t protocolNumber)
{
    return SendFrom(packet, GetAddress(), dest, protocolNumber);
}

bool
NvSwitch::SendFrom(Ptr<Packet> packet, const Address& source, const Address& dest,
                   uint16_t protocolNumber)
{
    NS_LOG_FUNCTION(this << packet << source << dest << protocolNumber);

    Mac48Address srcAddr = Mac48Address::ConvertFrom(source);
    Mac48Address dstAddr = Mac48Address::ConvertFrom(dest);

    // Look up output port
    int32_t outPort = LookupOutputPort(dstAddr);

    if (outPort >= 0 && static_cast<uint32_t>(outPort) < m_ports.size())
    {
        ForwardPacket(packet, srcAddr, dstAddr, outPort);
        return true;
    }

    // Flood to all ports
    for (size_t i = 0; i < m_ports.size(); ++i)
    {
        m_ports[i]->SendFrom(packet->Copy(), srcAddr, dstAddr, protocolNumber);
    }

    return true;
}

Ptr<Node>
NvSwitch::GetNode() const
{
    return m_node;
}

void
NvSwitch::SetNode(Ptr<Node> node)
{
    m_node = node;
}

bool
NvSwitch::NeedsArp() const
{
    return false;
}

void
NvSwitch::SetReceiveCallback(NetDevice::ReceiveCallback cb)
{
    m_rxCallback = cb;
}

void
NvSwitch::SetPromiscReceiveCallback(PromiscReceiveCallback cb)
{
    m_promiscRxCallback = cb;
}

bool
NvSwitch::SupportsSendFrom() const
{
    return true;
}

void
NvSwitch::ProcessAllGather(Ptr<Packet> packet, const FabricHeader& header,
                            uint32_t inPort)
{
    NS_LOG_FUNCTION(this << packet << inPort);

    uint16_t flowId = header.GetFlowId();
    uint16_t srcRank = header.GetSourceRank();

    // Dedup gate: mirror ProcessAllReduce — a retransmitted upload must not
    // advance the aggregation ledgers (re-ACK only).
    if (!RegisterUploadSeq(flowId, srcRank, header.GetSequenceNumber()))
    {
        SendSharpLlrAck(header, inPort);
        return;
    }

    // Learn port-to-rank mapping
    m_portRankMap[inPort] = srcRank;

    // Track bytes received per (flowId, srcRank) — effective bytes only,
    // same rule as ProcessAllReduce (codex round 19, finding 3): padding
    // fragments stamp zero effective bytes and must contribute zero, not
    // their wire payload.
    uint32_t effectiveSize = header.GetEffectiveDataSize();
    m_allGatherReceivedBytes[flowId][srcRank] += effectiveSize;

    // LLR ack the accepted upload (see ProcessAllReduce's SendSharpLlrAck note).
    SendSharpLlrAck(header, inPort);

    // Store chunk sizes only (not full Packet objects) to avoid memory explosion.
    m_allGatherBuffer[flowId][srcRank].push_back(effectiveSize);

    uint32_t sourceCount = m_allGatherReceivedBytes[flowId].size();

    NS_LOG_DEBUG("NVLS AllGather: flowId=" << flowId << " srcRank=" << srcRank
                 << " sources=" << sourceCount << "/" << m_allGatherThreshold
                 << " bytes=" << m_allGatherReceivedBytes[flowId][srcRank]);

    // Check if all GPUs have sent their FULL chunk. The injectors upload the
    // frozen chunk grid — base dataSize/numGpus plus one extra byte for the
    // first dataSize%numGpus ranks (NvlsAllGather's TriggerSendPhase) — so a
    // source's expected bytes are chunk + (src < remainder ? 1 : 0), not the
    // uniform chunkSize. Requiring only the uniform floor let a 4-GPU / 9-B
    // AllGather aggregate after the switch saw 2 bytes from rank 0 (its
    // third byte still in flight on a delayed lane): the multicast carried
    // 8 bytes, the ledger was retired, and the trailing byte started a new
    // aggregation that never completed — every receiver stuck at 8/9 (codex
    // round 18, finding 3). When dataSize was set, derive the per-source
    // expectation from it; fall back to the uniform chunk otherwise.
    //
    // Degenerate grid (codex round 19, finding 6): ranks at or past dataSize
    // hold a ZERO-sized chunk and never upload — sourceCount never reaches
    // the threshold and the aggregation stalled to the 1-hour cap (D=3/N=4
    // stopped at 3/4 sources; D=1 at 1/4). When dataSize is known, a rank
    // with expected == 0 is satisfied by construction, so the gate counts
    // sources with actual traffic plus zero-chunk ranks that owe nothing.
    bool allComplete = false;
    if (m_allGatherChunkSize > 0)
    {
        uint64_t numGpus = m_allGatherThreshold;
        uint64_t satisfied = sourceCount;
        if (m_allGatherDataSize > 0 && m_allGatherDataSize < numGpus)
        {
            satisfied += numGpus - m_allGatherDataSize; // zero-chunk ranks owe nothing
        }
        if (satisfied >= numGpus)
        {
            allComplete = true;
            for (const auto& src : m_allGatherReceivedBytes[flowId])
            {
                uint64_t expected = m_allGatherChunkSize;
                if (m_allGatherDataSize > 0)
                {
                    expected += (src.first < (m_allGatherDataSize % numGpus)) ? 1 : 0;
                }
                if (src.second < expected)
                {
                    allComplete = false;
                    break;
                }
            }
        }
    }
    else
    {
        uint64_t satisfied = sourceCount;
        if (m_allGatherDataSize > 0 && m_allGatherDataSize < m_allGatherThreshold)
        {
            satisfied += m_allGatherThreshold - m_allGatherDataSize;
        }
        allComplete = (satisfied >= m_allGatherThreshold);
    }

    if (allComplete)
    {
        m_deferredResultEvents.push_back(
            Simulator::Schedule(NanoSeconds(m_allReduceAggregationDelayNs),
                                &NvSwitch::MulticastAllGatherResult, this,
                                flowId, sourceCount));
    }
}

void
NvSwitch::MulticastAllGatherResult(uint16_t flowId, uint32_t sourceCount)
{
    NS_LOG_FUNCTION(this << flowId << sourceCount);

    auto& gpuBuffers = m_allGatherBuffer[flowId];

    // Collect ALL chunk sizes from ALL GPUs (concatenation of all chunks)
    std::vector<uint32_t> allChunkSizes;
    for (auto& gpu : gpuBuffers)
    {
        for (uint32_t sz : gpu.second)
        {
            allChunkSizes.push_back(sz);
        }
    }

    gpuBuffers.clear();

    // Group ports by GPU rank for spraying
    std::unordered_map<uint16_t, std::vector<uint32_t>> rankPorts = BuildRankPorts();

    // Multicast all concatenated chunks to each GPU. Create new packets from
    // stored chunk sizes (data content is dummy — only size matters for timing).
    for (auto& rankPortList : rankPorts)
    {
        uint16_t destRank = rankPortList.first;
        const auto& ports = rankPortList.second;

        for (size_t chunkIdx = 0; chunkIdx < allChunkSizes.size(); ++chunkIdx)
        {
            uint32_t thisChunkSize = allChunkSizes[chunkIdx];

            FabricHeader resultHeader;
            resultHeader.SetPacketType(FabricPacketType::ALLGATHER);
            resultHeader.SetFlowId(flowId);
            resultHeader.SetSourceRank(0xFFFF);
            resultHeader.SetDestRank(destRank);
            resultHeader.SetPayloadSize(thisChunkSize);
            resultHeader.SetEffectiveDataSize(thisChunkSize);
            // Per-(dest,flow) seq: reorder buffers expect each
            // (vc,flow,source) stream to start at 0 (see m_multicastSeqNums'
            // note).
            resultHeader.SetSequenceNumber(
                m_multicastSeqNums[(uint64_t{destRank} << 16) | flowId]++);
            m_multicastSeqNum++;

            Ptr<Packet> pkt = Create<Packet>(thisChunkSize);
            pkt->AddHeader(resultHeader);

            uint32_t portIdx = ports[chunkIdx % ports.size()];
            StoreMulticastResult(destRank, flowId, portIdx,
                                 resultHeader.GetSequenceNumber(), pkt);
            SendMulticastResultOnPort(portIdx, pkt);
        }
    }

    NS_LOG_INFO("NVLS AllGather: multicast " << allChunkSizes.size()
                << " result chunks for flowId=" << flowId
                << " to " << rankPorts.size() << " GPUs");

    // Flow complete: retire the byte ledger. m_seenUploadSeqs deliberately
    // survives the result multicast (see the AllReduce path's note): a late
    // replay of an accepted (flow, src, seq) must stay a rejected duplicate,
    // not re-accumulate the ledger into a second result.
    m_allGatherReceivedBytes.erase(flowId);
}

void
NvSwitch::SendSharpLlrAck(const FabricHeader& uploadHeader, uint32_t inPort)
{
    // No LLR on the fabric -> the sender has no retry entry to purge, and an
    // unsolicited ack would be counted by the endpoint as a retry miss.
    if (!m_llrEnabled)
    {
        return;
    }
    if (inPort >= m_ports.size())
    {
        return;
    }
    FabricHeader ackHeader;
    ackHeader.SetPacketType(FabricPacketType::ACK);
    ackHeader.SetFabricType(uploadHeader.GetFabricType());
    // SourceRank mirrors the upload's DestRank (the switch-addressed
    // destination): the endpoint's ProcessAckPacket removes its stored entry
    // under (seq, dest, flow, vc) — the same key space the switch's BER NACK
    // already uses — so this must NOT use the 0xFFFF result sentinel.
    ackHeader.SetSourceRank(uploadHeader.GetDestRank());
    ackHeader.SetDestRank(uploadHeader.GetSourceRank());
    ackHeader.SetSequenceNumber(uploadHeader.GetSequenceNumber());
    ackHeader.SetFlowId(uploadHeader.GetFlowId());
    ackHeader.SetVirtualChannel(uploadHeader.GetVirtualChannel());

    Ptr<Packet> ackPkt = Create<Packet>(0);
    ackPkt->AddHeader(ackHeader);
    m_ports[inPort]->Send(ackPkt, Mac48Address::GetBroadcast(), 0x0800);
}

void
NvSwitch::SendMulticastResultOnPort(uint32_t portIdx, Ptr<Packet> pkt)
{
    if (portIdx >= m_ports.size() || !m_ports[portIdx])
    {
        return;
    }

    // FEC egress for the multicast result path. These sends are direct port
    // Sends, so without this they bypass FEC entirely: with RS(544,514,15) a
    // 65,536-B result fragment serialized as 65,536 wire bytes where
    // GetEncodedSize requires 70,040 (codex round 12, finding 7). Mirror the
    // endpoint egress convention exactly: pad the body to the encoded size
    // (the FEC receiver strips parity via header.GetPayloadSize()), and hold
    // the send for the codec latency like ForwardPacket's scheduled path.
    // The un-padded packet still goes to StoreMulticastResult: repairs
    // re-expand from the header's payload size on their own pass through
    // here.
    uint64_t wireSize = pkt->GetSize();
    Time codingLatency = Seconds(0);
    if (m_fecModel && m_fecModel->IsEnabled() && PortUsesFec(portIdx))
    {
        FabricHeader header;
        if (pkt->PeekHeader(header))
        {
            uint32_t payloadSize = pkt->GetSize() - header.GetSerializedSize();
            uint32_t encodedPayloadSize = m_fecModel->GetEncodedSize(payloadSize);
            if (encodedPayloadSize > payloadSize)
            {
                uint32_t padding = encodedPayloadSize - payloadSize;
                pkt->AddAtEnd(Create<Packet>(padding));
                wireSize = header.GetSerializedSize() + encodedPayloadSize;
            }
            codingLatency = m_fecModel->GetEncodeLatency()
                            + m_fecModel->GetDecodeLatency();
        }
    }
    m_txTrace(pkt);
    m_txPackets++;
    m_txBytes += wireSize;

    if (codingLatency > Seconds(0))
    {
        m_deferredResultEvents.push_back(
            Simulator::Schedule(codingLatency, [this, portIdx, pkt]() {
                if (portIdx < m_ports.size() && m_ports[portIdx])
                {
                    m_ports[portIdx]->Send(pkt, Mac48Address::GetBroadcast(), 0x0800);
                }
            }));
    }
    else
    {
        m_ports[portIdx]->Send(pkt, Mac48Address::GetBroadcast(), 0x0800);
    }
}

void
NvSwitch::StoreMulticastResult(uint16_t destRank, uint16_t flowId,
                               uint32_t portIdx, uint32_t seqNum, Ptr<Packet> pkt)
{
    // Only needed for NACK-driven retransmit; skip when LLR is off (results
    // are best-effort in that regime, matching legacy behavior).
    if (!m_llrEnabled)
    {
        return;
    }
    // Keep the stamped header only — the body is dummy bytes (only sizes
    // matter for timing), so a retransmit rebuilds it from the header's
    // payload size. A full copy per fragment forced the old 512-entry window,
    // which evicted fragments before their first delivery at real scales
    // (1,024 fragments/dest on an 8-GPU 128-MiB allgather).
    FabricHeader header;
    if (!pkt->PeekHeader(header))
    {
        return;
    }
    // Entries belong to a FLOW and the store is keyed per (dest, flow), so
    // concurrent flows keep INDEPENDENT windows — each bounded by
    // SHARP_RESULT_WINDOW on its own. The old wholesale erase of the
    // previous flow's window assumed "a new flow's results imply the old
    // flow completed", but completion is not DELIVERY: flow 2 could
    // aggregate and multicast while flow 1's results are still in flight,
    // and the erase destroyed flow 1's repair state — a later NACK for
    // flow 1/seq 0 then fell back to flow 2's window and retransmitted the
    // WRONG flow's fragment while flow 1's loss stayed unrecoverable
    // (codex round 24, finding 4). Eviction of an old flow's window now
    // happens only through its own bound (the safety valve) or DoDispose;
    // seq streams are per-(dest, flow), so cross-flow key collisions are
    // impossible.
    uint64_t key = (static_cast<uint64_t>(destRank) << 16) | flowId;
    auto& window = m_sharpResultStore[key];
    m_lastResultFlow[destRank] = flowId;
    window.push_back({seqNum, portIdx, 0, header});
    while (window.size() > SHARP_RESULT_WINDOW)
    {
        window.pop_front();
    }
}

bool
NvSwitch::HandleSwitchNack(const FabricHeader& nackHeader, uint32_t inPort)
{
    (void)inPort;
    uint16_t requestingRank = nackHeader.GetSourceRank();
    uint32_t seqNum = nackHeader.GetSequenceNumber();

    // The store is keyed per (dest, flow) and the NACK NAMES the flow it
    // lost a fragment of: repair must serve THAT flow's window only. The
    // old fallback to the dest's last stored flow silently re-keyed a
    // flow-1 NACK onto flow 2's window after flow 1's entries were dropped
    // — retransmitting the wrong flow's fragment while the requested one
    // stayed unrecoverable (codex round 24, finding 4). A named-flow miss
    // (stale/evicted/bogus flow id) is a genuine repair failure: false.
    uint16_t flowId = nackHeader.GetFlowId();
    auto destIt = m_sharpResultStore.find(
        (static_cast<uint64_t>(requestingRank) << 16) | flowId);
    if (destIt == m_sharpResultStore.end())
    {
        return false;
    }
    auto& window = destIt->second;
    for (auto& entry : window)
    {
        if (entry.seqNum != seqNum)
        {
            continue;
        }
        if (entry.portIdx >= m_ports.size() || !IsPortOperational(entry.portIdx))
        {
            return false;
        }
        if (entry.retransmits >= SHARP_RESULT_RETRY_LIMIT)
        {
            NS_LOG_WARN("Switch result retransmit limit reached for seq "
                        << seqNum << " destRank " << requestingRank);
            return false;
        }
        entry.retransmits++;
        // Rebuild the body (dummy bytes — only the size matters) and stamp
        // the stored header so the retransmit is wire-identical in shape.
        // Re-expands FEC parity on FEC ports like the original multicast.
        Ptr<Packet> pkt = Create<Packet>(entry.header.GetPayloadSize());
        pkt->AddHeader(entry.header);
        SendMulticastResultOnPort(entry.portIdx, pkt);
        NS_LOG_DEBUG("Switch retransmit multicast result seq " << seqNum
                     << " to rank " << requestingRank);
        return true;
    }
    return false;
}

} // namespace ns3
