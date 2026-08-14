/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Gateway Endpoint for cross-fabric routing in hybrid GPU clusters
 */

#include "gateway-endpoint.h"
#include "fabric-header.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "ns3/pointer.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("GatewayEndpoint");

NS_OBJECT_ENSURE_REGISTERED(GatewayEndpoint);

TypeId
GatewayEndpoint::GetTypeId()
{
    static TypeId tid = TypeId("ns3::GatewayEndpoint")
        .SetParent<FabricEndpoint>()
        .SetGroupName("GpuCluster")
        .AddConstructor<GatewayEndpoint>()
        .AddAttribute("GatewayDelay",
                      "Processing delay for cross-fabric forwarding",
                      TimeValue(MicroSeconds(1)),
                      MakeTimeAccessor(&GatewayEndpoint::m_gatewayDelay),
                      MakeTimeChecker())
        .AddAttribute("MaxHops",
                      "Maximum forwarding hops to prevent loops",
                      UintegerValue(8),
                      MakeUintegerAccessor(&GatewayEndpoint::m_maxHops),
                      MakeUintegerChecker<uint8_t>())
    ;
    return tid;
}

GatewayEndpoint::GatewayEndpoint()
    : m_gatewayDelay(MicroSeconds(1)),
      m_maxHops(8)
{
    NS_LOG_FUNCTION(this);
}

GatewayEndpoint::~GatewayEndpoint()
{
    NS_LOG_FUNCTION(this);
}

void
GatewayEndpoint::DoDispose()
{
    NS_LOG_FUNCTION(this);
    m_hybridRoutingTable = nullptr;
    m_fabricDeviceMap.clear();
    FabricEndpoint::DoDispose();
}

void
GatewayEndpoint::StartApplication()
{
    NS_LOG_FUNCTION(this);
    FabricEndpoint::StartApplication();
}

void
GatewayEndpoint::StopApplication()
{
    NS_LOG_FUNCTION(this);
    FabricEndpoint::StopApplication();
}

void
GatewayEndpoint::AddFabricDevice(FabricType fabricType, Ptr<NetDevice> device)
{
    NS_LOG_FUNCTION(this << static_cast<int>(fabricType) << device);

    // Add to parent's device list
    uint32_t deviceIndex = GetNNetDevices();
    AddNetDevice(device);

    // Record the mapping
    m_fabricDeviceMap[fabricType] = deviceIndex;

    NS_LOG_DEBUG("Added fabric device: type=" << static_cast<int>(fabricType)
                  << " deviceIndex=" << deviceIndex);
}

Ptr<NetDevice>
GatewayEndpoint::GetFabricDevice(FabricType fabricType) const
{
    auto it = m_fabricDeviceMap.find(fabricType);
    if (it != m_fabricDeviceMap.end())
    {
        return GetNetDevice(it->second);
    }
    return nullptr;
}

void
GatewayEndpoint::SetHybridRoutingTable(Ptr<HybridRoutingTable> table)
{
    NS_LOG_FUNCTION(this << table);
    m_hybridRoutingTable = table;
}

Ptr<HybridRoutingTable>
GatewayEndpoint::GetHybridRoutingTable() const
{
    return m_hybridRoutingTable;
}

void
GatewayEndpoint::SetGatewayDelay(Time delay)
{
    NS_LOG_FUNCTION(this << delay);
    m_gatewayDelay = delay;
}

Time
GatewayEndpoint::GetGatewayDelay() const
{
    return m_gatewayDelay;
}

void
GatewayEndpoint::SetCrossFabricRoute(uint16_t destRank, FabricType outFabric, uint32_t deviceIndex)
{
    NS_LOG_FUNCTION(this << destRank << static_cast<int>(outFabric) << deviceIndex);

    if (m_hybridRoutingTable)
    {
        RouteEntry entry;
        entry.fabric = outFabric;
        entry.deviceIndex = deviceIndex;
        entry.gatewayRank = GetRank();
        entry.isCrossFabric = true;
        m_hybridRoutingTable->AddRoute(destRank, entry);
    }
    else
    {
        NS_LOG_WARN("No hybrid routing table set, cannot add cross-fabric route");
    }
}

uint32_t
GatewayEndpoint::GetNFabrics() const
{
    return static_cast<uint32_t>(m_fabricDeviceMap.size());
}

void
GatewayEndpoint::ProcessDataPacketWithForwarding(Ptr<Packet> packet, const FabricHeader& header,
                                                   FabricType incomingFabric)
{
    NS_LOG_FUNCTION(this << packet << header.GetDestRank() << static_cast<int>(incomingFabric));

    uint16_t destRank = header.GetDestRank();

    // Check if this packet is for us
    if (destRank == GetRank())
    {
        // Deliver locally - use parent's processing
        // This would normally be called through the parent's ReceiveFromDevice
        NS_LOG_DEBUG("Packet for local delivery");
        return;
    }

    // Check if we should forward
    if (!m_hybridRoutingTable)
    {
        NS_LOG_WARN("No routing table, cannot forward");
        return;
    }

    auto route = m_hybridRoutingTable->LookupRoute(destRank);
    if (!route)
    {
        NS_LOG_WARN("No route to destination " << destRank);
        return;
    }

    FabricType outFabric = route->fabric;
    uint32_t outDeviceIndex = route->deviceIndex;

    // Check for potential forwarding loop
    if (outFabric == incomingFabric && outDeviceIndex == route->deviceIndex)
    {
        NS_LOG_ERROR("Forwarding loop detected: same fabric and device");
        return;
    }

    NS_LOG_DEBUG("Forwarding from fabric " << static_cast<int>(incomingFabric)
                  << " to fabric " << static_cast<int>(outFabric)
                  << " deviceIndex=" << outDeviceIndex);

    // Schedule forwarding with gateway delay
    ScheduleForwarding(packet, header, outFabric, outDeviceIndex);
}

void
GatewayEndpoint::ForwardToFabric(Ptr<Packet> packet, FabricHeader header,
                                  FabricType outFabric, uint32_t outDeviceIndex)
{
    NS_LOG_FUNCTION(this << static_cast<int>(outFabric) << outDeviceIndex);

    // Update fabric type in header
    header.SetFabricType(outFabric);

    // Re-serialize the header
    packet->RemoveHeader(header);
    header.SetFabricType(outFabric);
    packet->AddHeader(header);

    // Get the destination MAC address
    Mac48Address destMac = GetNeighborMac(header.GetDestRank());
    if (destMac == Mac48Address())
    {
        // Use broadcast if unknown
        destMac = Mac48Address::GetBroadcast();
    }

    // Send on the appropriate device
    if (outDeviceIndex < GetNNetDevices())
    {
        SendPacketOnDevice(packet, outDeviceIndex, destMac);
    }
    else
    {
        NS_LOG_ERROR("Invalid device index: " << outDeviceIndex);
    }
}

void
GatewayEndpoint::ScheduleForwarding(Ptr<Packet> packet, FabricHeader header,
                                     FabricType outFabric, uint32_t outDeviceIndex)
{
    NS_LOG_FUNCTION(this);

    // Schedule the forwarding after gateway delay
    Simulator::Schedule(m_gatewayDelay,
                        &GatewayEndpoint::ForwardToFabric,
                        this, packet, header, outFabric, outDeviceIndex);
}

} // namespace ns3
