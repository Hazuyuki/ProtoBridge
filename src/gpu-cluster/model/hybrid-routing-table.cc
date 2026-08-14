/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Hybrid routing table for multi-fabric GPU clusters
 */

#include "hybrid-routing-table.h"
#include "ns3/log.h"
#include "ns3/type-id.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("HybridRoutingTable");

NS_OBJECT_ENSURE_REGISTERED(HybridRoutingTable);

TypeId
HybridRoutingTable::GetTypeId()
{
    static TypeId tid = TypeId("ns3::HybridRoutingTable")
        .SetParent<Object>()
        .SetGroupName("GpuCluster")
        .AddConstructor<HybridRoutingTable>()
    ;
    return tid;
}

HybridRoutingTable::HybridRoutingTable()
{
    NS_LOG_FUNCTION(this);
}

HybridRoutingTable::~HybridRoutingTable()
{
    NS_LOG_FUNCTION(this);
}

void
HybridRoutingTable::AddRoute(uint16_t destRank, const RouteEntry& entry)
{
    NS_LOG_FUNCTION(this << destRank);
    m_routes[destRank] = entry;
}

std::optional<RouteEntry>
HybridRoutingTable::LookupRoute(uint16_t destRank) const
{
    NS_LOG_FUNCTION(this << destRank);
    auto it = m_routes.find(destRank);
    if (it != m_routes.end())
    {
        return it->second;
    }
    return std::nullopt;
}

void
HybridRoutingTable::RemoveRoute(uint16_t destRank)
{
    NS_LOG_FUNCTION(this << destRank);
    m_routes.erase(destRank);
}

void
HybridRoutingTable::Clear()
{
    NS_LOG_FUNCTION(this);
    m_routes.clear();
    m_fabricGateways.clear();
}

void
HybridRoutingTable::SetGatewayForFabric(FabricType fabricType, uint16_t gatewayRank)
{
    NS_LOG_FUNCTION(this << static_cast<int>(fabricType) << gatewayRank);
    m_fabricGateways[static_cast<uint8_t>(fabricType)] = gatewayRank;
}

uint16_t
HybridRoutingTable::GetGatewayForFabric(FabricType fabricType) const
{
    NS_LOG_FUNCTION(this << static_cast<int>(fabricType));
    auto it = m_fabricGateways.find(static_cast<uint8_t>(fabricType));
    if (it != m_fabricGateways.end())
    {
        return it->second;
    }
    return 0;
}

bool
HybridRoutingTable::HasRoute(uint16_t destRank) const
{
    return m_routes.find(destRank) != m_routes.end();
}

uint32_t
HybridRoutingTable::GetNRoutes() const
{
    return static_cast<uint32_t>(m_routes.size());
}

} // namespace ns3
