/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Hybrid routing table for multi-fabric GPU clusters
 */

#ifndef HYBRID_ROUTING_TABLE_H
#define HYBRID_ROUTING_TABLE_H

#include "fabric-type.h"

#include "ns3/object.h"
#include "ns3/mac48-address.h"

#include <unordered_map>
#include <optional>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Route entry for hybrid routing table
 */
struct RouteEntry
{
    FabricType fabric;       ///< Which fabric to use
    uint32_t deviceIndex;    ///< Device index on that fabric
    uint16_t gatewayRank;    ///< Next hop gateway (if cross-fabric)
    bool isCrossFabric;      ///< True if destination in different fabric

    RouteEntry()
        : fabric(FabricType::NVLINK),
          deviceIndex(0),
          gatewayRank(0),
          isCrossFabric(false)
    {}

    RouteEntry(FabricType f, uint32_t devIdx, uint16_t gw = 0, bool crossFabric = false)
        : fabric(f),
          deviceIndex(devIdx),
          gatewayRank(gw),
          isCrossFabric(crossFabric)
    {}
};

/**
 * @ingroup gpu-cluster
 * @brief Hybrid routing table for multi-fabric GPU clusters
 *
 * This class manages routing information for destinations across
 * multiple fabrics (NVLink, Ethernet). It supports:
 * - Direct routes within a fabric
 * - Cross-fabric routes via gateways
 * - Per-fabric gateway configuration
 */
class HybridRoutingTable : public Object
{
  public:
    static TypeId GetTypeId();

    HybridRoutingTable();
    ~HybridRoutingTable() override;

    /**
     * @brief Add a route to a destination
     * @param destRank Destination rank
     * @param entry Route entry
     */
    void AddRoute(uint16_t destRank, const RouteEntry& entry);

    /**
     * @brief Lookup a route to a destination
     * @param destRank Destination rank
     * @return Route entry if found, nullopt otherwise
     */
    std::optional<RouteEntry> LookupRoute(uint16_t destRank) const;

    /**
     * @brief Remove a route
     * @param destRank Destination rank
     */
    void RemoveRoute(uint16_t destRank);

    /**
     * @brief Clear all routes
     */
    void Clear();

    /**
     * @brief Set the gateway rank for a specific fabric
     * @param fabricType Fabric type
     * @param gatewayRank Gateway rank
     */
    void SetGatewayForFabric(FabricType fabricType, uint16_t gatewayRank);

    /**
     * @brief Get the gateway rank for a specific fabric
     * @param fabricType Fabric type
     * @return Gateway rank, or 0 if not set
     */
    uint16_t GetGatewayForFabric(FabricType fabricType) const;

    /**
     * @brief Check if a route exists
     * @param destRank Destination rank
     * @return True if route exists
     */
    bool HasRoute(uint16_t destRank) const;

    /**
     * @brief Get number of routes
     * @return Number of routes in the table
     */
    uint32_t GetNRoutes() const;

  private:
    std::unordered_map<uint16_t, RouteEntry> m_routes; ///< destRank -> RouteEntry
    std::unordered_map<uint8_t, uint16_t> m_fabricGateways; ///< fabricType -> gatewayRank
};

} // namespace ns3

#endif /* HYBRID_ROUTING_TABLE_H */
