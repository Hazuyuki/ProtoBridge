/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * NVSwitch Hardware Model Implementation
 */

#include "nvswitch.h"

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
    m_ports.clear();
    m_voqs.clear();
    m_portDegradationModels.clear();
    m_macTable.clear();
    m_sourceBasedTable.clear();
    m_sprayPortsTable.clear();
    m_sprayRoundRobin.clear();
    m_allReduceBuffer.clear();
    m_allReduceReceivedBytes.clear();
    m_portRankMap.clear();
    m_allGatherBuffer.clear();
    m_allGatherReceivedBytes.clear();
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
    // RETRY_ACK) are reliable against BER corruption — matching real NVLink
    // behavior where control traffic bypasses CRC/FEC checks.
    // Only data/collective/p2p packets go through BER sampling.
    bool isControlPacket = false;
    if (hasFabricHeader)
    {
        FabricPacketType pktType = fabricHeader.GetPacketType();
        if (pktType == FabricPacketType::CREDIT ||
            pktType == FabricPacketType::ACK ||
            pktType == FabricPacketType::NACK ||
            pktType == FabricPacketType::RETRY_REQUEST ||
            pktType == FabricPacketType::RETRY_ACK ||
            pktType == FabricPacketType::PERMANENT_LOSS)
        {
            isControlPacket = true;
        }
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
            failed = !linkModel->ProcessPacket(pkt);
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
    if (p2pOut && m_cutThroughDelayNs > 0)
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

    // Learn port-to-rank mapping for multicast
    m_portRankMap[inPort] = srcRank;

    // Track bytes received per (flowId, srcRank)
    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = header.GetPayloadSize();
    }
    m_allReduceReceivedBytes[flowId][srcRank] += effectiveSize;

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
    uint32_t numPartitions = m_allReduceNumPartitions;
    if (numPartitions == 0 && m_allReduceThreshold > 0)
    {
        numPartitions = m_allReduceThreshold; // default: partition by numGpus
    }

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
            uint64_t ratioWhole = receivedWireBytes / receivedEffectiveBytes;
            uint64_t ratioRemainder = receivedWireBytes % receivedEffectiveBytes;
            uint64_t wirePartitionSize =
                thisPartitionSize * ratioWhole +
                (thisPartitionSize * ratioRemainder +
                 receivedEffectiveBytes - 1) /
                    receivedEffectiveBytes;
            Simulator::Schedule(NanoSeconds(m_allReduceAggregationDelayNs),
                                &NvSwitch::MulticastAllReducePartition, this,
                                flowId, k, thisPartitionSize, wirePartitionSize,
                                numPartitions);
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

        if (allComplete)
        {
            Simulator::Schedule(NanoSeconds(m_allReduceAggregationDelayNs),
                                &NvSwitch::MulticastAllReduceResult, this,
                                flowId, sourceCount);
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
    // chunk sizes to create result packets of the same sizes.
    auto firstGpu = gpuBuffers.begin();
    const auto& chunkSizes = firstGpu->second;

    // Clear buffer before multicast
    gpuBuffers.clear();

    // Group ports by GPU rank for spraying chunks across multiple links per GPU
    std::unordered_map<uint16_t, std::vector<uint32_t>> rankPorts;
    for (const auto& entry : m_portRankMap)
    {
        rankPorts[entry.second].push_back(entry.first);
    }

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
            resultHeader.SetSequenceNumber(m_multicastSeqNum++);

            Ptr<Packet> pkt = Create<Packet>(thisChunkSize);
            pkt->AddHeader(resultHeader);

            // Spray chunks across this GPU's ports
            uint32_t portIdx = ports[chunkIdx % ports.size()];
            m_ports[portIdx]->Send(pkt, Mac48Address::GetBroadcast(), 0x0800);
        }
    }

    NS_LOG_INFO("SHARP AllReduce: multicast " << chunkSizes.size()
                << " result packets for flowId=" << flowId
                << " to " << rankPorts.size() << " GPUs");
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
    std::unordered_map<uint16_t, std::vector<uint32_t>> rankPorts;
    for (const auto& entry : m_portRankMap)
    {
        rankPorts[entry.second].push_back(entry.first);
    }

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
            resultHeader.SetSequenceNumber(m_multicastSeqNum++);

            pkt->AddHeader(resultHeader);

            uint32_t portIdx = ports[chunkIdx % ports.size()];
            m_ports[portIdx]->Send(pkt, Mac48Address::GetBroadcast(), 0x0800);
        }
    }

    // Clean up buffer after last partition is multicast
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

    // Learn port-to-rank mapping
    m_portRankMap[inPort] = srcRank;

    // Track bytes received per (flowId, srcRank)
    uint32_t effectiveSize = header.GetEffectiveDataSize();
    if (effectiveSize == 0)
    {
        effectiveSize = header.GetPayloadSize();
    }
    m_allGatherReceivedBytes[flowId][srcRank] += effectiveSize;

    // Store chunk sizes only (not full Packet objects) to avoid memory explosion.
    m_allGatherBuffer[flowId][srcRank].push_back(effectiveSize);

    uint32_t sourceCount = m_allGatherReceivedBytes[flowId].size();

    NS_LOG_DEBUG("NVLS AllGather: flowId=" << flowId << " srcRank=" << srcRank
                 << " sources=" << sourceCount << "/" << m_allGatherThreshold
                 << " bytes=" << m_allGatherReceivedBytes[flowId][srcRank]);

    // Check if all GPUs have sent their chunk
    bool allComplete = false;
    if (m_allGatherChunkSize > 0)
    {
        if (sourceCount >= m_allGatherThreshold)
        {
            allComplete = true;
            for (const auto& src : m_allGatherReceivedBytes[flowId])
            {
                if (src.second < m_allGatherChunkSize)
                {
                    allComplete = false;
                    break;
                }
            }
        }
    }
    else
    {
        allComplete = (sourceCount >= m_allGatherThreshold);
    }

    if (allComplete)
    {
        Simulator::Schedule(NanoSeconds(m_allReduceAggregationDelayNs),
                            &NvSwitch::MulticastAllGatherResult, this,
                            flowId, sourceCount);
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
    std::unordered_map<uint16_t, std::vector<uint32_t>> rankPorts;
    for (const auto& entry : m_portRankMap)
    {
        rankPorts[entry.second].push_back(entry.first);
    }

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
            resultHeader.SetSequenceNumber(m_multicastSeqNum++);

            Ptr<Packet> pkt = Create<Packet>(thisChunkSize);
            pkt->AddHeader(resultHeader);

            uint32_t portIdx = ports[chunkIdx % ports.size()];
            m_ports[portIdx]->Send(pkt, Mac48Address::GetBroadcast(), 0x0800);
        }
    }

    NS_LOG_INFO("NVLS AllGather: multicast " << allChunkSizes.size()
                << " result chunks for flowId=" << flowId
                << " to " << rankPorts.size() << " GPUs");
}

} // namespace ns3
