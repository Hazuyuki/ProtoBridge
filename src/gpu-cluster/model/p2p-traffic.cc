/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 */

#include "p2p-traffic.h"
#include "fabric-endpoint.h"

#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("P2pTraffic");

NS_OBJECT_ENSURE_REGISTERED(P2pTraffic);

TypeId
P2pTraffic::GetTypeId()
{
    static TypeId tid = TypeId("ns3::P2pTraffic")
                            .SetParent<TrafficPattern>()
                            .SetGroupName("GpuCluster")
                            .AddConstructor<P2pTraffic>()
                            .AddAttribute("DestRank",
                                          "Destination rank",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&P2pTraffic::m_destRank),
                                          MakeUintegerChecker<uint16_t>())
                            .AddAttribute("PacketSize",
                                          "Packet payload size in bytes",
                                          UintegerValue(1024),
                                          MakeUintegerAccessor(&P2pTraffic::m_packetSize),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("Interval",
                                          "Time between packets",
                                          TimeValue(MicroSeconds(10)),
                                          MakeTimeAccessor(&P2pTraffic::m_interval),
                                          MakeTimeChecker())
                            .AddAttribute("MaxPackets",
                                          "Maximum number of packets to send (0 = unlimited)",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&P2pTraffic::m_maxPackets),
                                          MakeUintegerChecker<uint32_t>())
                            .AddAttribute("FlowId",
                                          "Flow ID for P2P traffic",
                                          UintegerValue(1),
                                          MakeUintegerAccessor(&P2pTraffic::m_flowId),
                                          MakeUintegerChecker<uint16_t>())
                            .AddAttribute("VcId",
                                          "Virtual channel ID",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&P2pTraffic::m_vcId),
                                          MakeUintegerChecker<uint8_t>())
                            .AddAttribute("BurstSize",
                                          "Number of packets per burst (0 = single packet)",
                                          UintegerValue(0),
                                          MakeUintegerAccessor(&P2pTraffic::m_burstSize),
                                          MakeUintegerChecker<uint32_t>());
    return tid;
}

P2pTraffic::P2pTraffic()
    : m_destRank(0),
      m_packetSize(1024),
      m_interval(MicroSeconds(10)),
      m_maxPackets(0),
      m_flowId(1),
      m_vcId(0),
      m_burstSize(0),
      m_packetsSent(0),
      m_bytesSent(0)
{
}

P2pTraffic::~P2pTraffic()
{
}

void
P2pTraffic::SetDestRank(uint16_t rank)
{
    m_destRank = rank;
}

uint16_t
P2pTraffic::GetDestRank() const
{
    return m_destRank;
}

void
P2pTraffic::SetPacketSize(uint32_t size)
{
    m_packetSize = size;
}

uint32_t
P2pTraffic::GetPacketSize() const
{
    return m_packetSize;
}

void
P2pTraffic::SetInterval(Time interval)
{
    m_interval = interval;
}

Time
P2pTraffic::GetInterval() const
{
    return m_interval;
}

void
P2pTraffic::SetMaxPackets(uint32_t count)
{
    m_maxPackets = count;
}

uint32_t
P2pTraffic::GetMaxPackets() const
{
    return m_maxPackets;
}

void
P2pTraffic::SetFlowId(uint16_t flowId)
{
    m_flowId = flowId;
}

uint16_t
P2pTraffic::GetFlowId() const
{
    return m_flowId;
}

void
P2pTraffic::SetVcId(uint8_t vcId)
{
    m_vcId = vcId;
}

uint8_t
P2pTraffic::GetVcId() const
{
    return m_vcId;
}

void
P2pTraffic::SetBurstSize(uint32_t burstSize)
{
    m_burstSize = burstSize;
}

uint32_t
P2pTraffic::GetBurstSize() const
{
    return m_burstSize;
}

uint32_t
P2pTraffic::GetPacketsSent() const
{
    return m_packetsSent;
}

uint64_t
P2pTraffic::GetBytesSent() const
{
    return m_bytesSent;
}

void
P2pTraffic::DoStart()
{
    NS_LOG_FUNCTION(this);
    m_packetsSent = 0;
    m_bytesSent = 0;
    SendBurst();
}

void
P2pTraffic::DoStop()
{
    NS_LOG_FUNCTION(this);
    if (m_sendEvent.IsPending())
    {
        Simulator::Cancel(m_sendEvent);
    }
}

void
P2pTraffic::SendBurst()
{
    if (!m_running || !m_endpoint)
    {
        return;
    }

    uint32_t burstCount = (m_burstSize > 0) ? m_burstSize : 1;

    for (uint32_t i = 0; i < burstCount; ++i)
    {
        if (m_maxPackets > 0 && m_packetsSent >= m_maxPackets)
        {
            NotifyComplete();
            return;
        }

        std::vector<uint8_t> data(m_packetSize, 0);
        m_endpoint->SendP2p(m_destRank, data.data(), m_packetSize, m_flowId, m_vcId);
        m_packetsSent++;
        m_bytesSent += m_packetSize;
    }

    ScheduleNext();
}

void
P2pTraffic::ScheduleNext()
{
    if (!m_running)
    {
        return;
    }

    if (m_maxPackets > 0 && m_packetsSent >= m_maxPackets)
    {
        NotifyComplete();
        return;
    }

    m_sendEvent = Simulator::Schedule(m_interval, &P2pTraffic::SendBurst, this);
}

} // namespace ns3
