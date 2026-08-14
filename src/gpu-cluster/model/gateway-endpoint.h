/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Gateway Endpoint for cross-fabric routing in hybrid GPU clusters
 */

#ifndef GATEWAY_ENDPOINT_H
#define GATEWAY_ENDPOINT_H

#include "fabric-endpoint.h"
#include "hybrid-routing-table.h"
#include "fabric-type.h"

#include "ns3/net-device.h"
#include "ns3/timer.h"

#include <unordered_map>
#include <map>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Gateway Endpoint for bridging multiple fabrics
 *
 * A GatewayEndpoint is a specialized FabricEndpoint that can bridge
 * traffic between different fabrics (e.g., NVLink to Ethernet).
 *
 * Key responsibilities:
 * - Forward packets between fabrics based on routing table
 * - Manage per-fabric credit managers
 * - Apply gateway processing delay
 * - Handle cross-fabric address translation
 */
class GatewayEndpoint : public FabricEndpoint
{
  public:
    static TypeId GetTypeId();

    GatewayEndpoint();
    ~GatewayEndpoint() override;

    /**
     * @brief Add a NetDevice for a specific fabric type
     * @param fabricType The fabric type (NVLINK, ETHERNET)
     * @param device The NetDevice to add
     */
    void AddFabricDevice(FabricType fabricType, Ptr<NetDevice> device);

    /**
     * @brief Get the NetDevice for a specific fabric type
     * @param fabricType The fabric type
     * @return The NetDevice, or nullptr if not set
     */
    Ptr<NetDevice> GetFabricDevice(FabricType fabricType) const;

    /**
     * @brief Set the hybrid routing table
     * @param table The routing table
     */
    void SetHybridRoutingTable(Ptr<HybridRoutingTable> table);

    /**
     * @brief Get the hybrid routing table
     * @return The routing table
     */
    Ptr<HybridRoutingTable> GetHybridRoutingTable() const;

    /**
     * @brief Set gateway processing delay
     * @param delay Processing delay for cross-fabric forwarding
     */
    void SetGatewayDelay(Time delay);

    /**
     * @brief Get gateway processing delay
     * @return Processing delay
     */
    Time GetGatewayDelay() const;

    /**
     * @brief Set the fabric type for an outgoing route
     * @param destRank Destination rank
     * @param outFabric Fabric to use for this destination
     * @param deviceIndex Device index on that fabric
     */
    void SetCrossFabricRoute(uint16_t destRank, FabricType outFabric, uint32_t deviceIndex);

    /**
     * @brief Get the number of fabrics configured
     * @return Number of fabrics
     */
    uint32_t GetNFabrics() const;

  protected:
    void DoDispose() override;

  private:
    void StartApplication() override;
    void StopApplication() override;

    /**
     * @brief Process incoming data packet with cross-fabric forwarding
     * @param packet The received packet
     * @param header The fabric header
     * @param incomingFabric The fabric on which packet arrived
     */
    void ProcessDataPacketWithForwarding(Ptr<Packet> packet, const FabricHeader& header,
                                          FabricType incomingFabric);

    /**
     * @brief Forward a packet to another fabric
     * @param packet The packet to forward
     * @param header The fabric header (will be modified)
     * @param outFabric The destination fabric
     * @param outDeviceIndex The device index on the destination fabric
     */
    void ForwardToFabric(Ptr<Packet> packet, FabricHeader header,
                         FabricType outFabric, uint32_t outDeviceIndex);

    /**
     * @brief Schedule forwarding after gateway delay
     * @param packet The packet to forward
     * @param header The fabric header
     * @param outFabric The destination fabric
     * @param outDeviceIndex The device index
     */
    void ScheduleForwarding(Ptr<Packet> packet, FabricHeader header,
                            FabricType outFabric, uint32_t outDeviceIndex);

    /// Map from fabric type to device index
    std::map<FabricType, uint32_t> m_fabricDeviceMap;

    /// Hybrid routing table for cross-fabric routes
    Ptr<HybridRoutingTable> m_hybridRoutingTable;

    /// Gateway processing delay
    Time m_gatewayDelay;

    /// Flag to prevent infinite forwarding loops
    uint8_t m_maxHops;
};

} // namespace ns3

#endif /* GATEWAY_ENDPOINT_H */
