/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Helper for building hybrid multi-fabric GPU cluster topologies
 */

#ifndef HYBRID_TOPOLOGY_HELPER_H
#define HYBRID_TOPOLOGY_HELPER_H

#include "ns3/node-container.h"
#include "ns3/application-container.h"
#include "ns3/ptr.h"
#include "ns3/fabric-type.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace ns3
{

class HybridRoutingTable;
class FabricEndpoint;
class LinkDegradationModel;

/**
 * @ingroup gpu-cluster
 * @brief Configuration for a single fabric domain
 */
struct FabricConfig
{
    uint16_t fabricId;          ///< Unique fabric identifier
    FabricType fabricType;      ///< Fabric type (NVLINK, ETHERNET)
    uint32_t numGpus;           ///< Number of GPUs in this fabric
    std::string topologyType;   ///< Topology type: "ring", "fullmesh", "switch"
    std::string dataRate;       ///< Link data rate
    std::string delay;          ///< Link delay
    uint16_t baseRank;          ///< Starting rank for GPUs in this fabric
    NodeContainer nodes;        ///< Nodes in this fabric
    ApplicationContainer endpoints; ///< Endpoint applications
    uint16_t gatewayRank;       ///< Rank of gateway node (if any)

    FabricConfig()
        : fabricId(0),
          fabricType(FabricType::NVLINK),
          numGpus(0),
          topologyType("ring"),
          dataRate("300Gbps"),
          delay("500ns"),
          baseRank(0),
          gatewayRank(0)
    {}
};

/**
 * @ingroup gpu-cluster
 * @brief Configuration for a cross-fabric link
 */
struct CrossFabricLink
{
    uint16_t fabric1Id;     ///< First fabric ID
    uint16_t fabric2Id;     ///< Second fabric ID
    uint16_t gateway1Rank;  ///< Gateway rank in fabric 1
    uint16_t gateway2Rank;  ///< Gateway rank in fabric 2
    std::string dataRate;   ///< Link data rate
    std::string delay;      ///< Link delay

    CrossFabricLink()
        : fabric1Id(0),
          fabric2Id(1),
          gateway1Rank(0),
          gateway2Rank(0),
          dataRate("100Gbps"),
          delay("10us")
    {}
};

/**
 * @ingroup gpu-cluster
 * @brief Helper to build hybrid multi-fabric GPU cluster topologies
 *
 * This helper enables building GPU clusters with multiple fabrics:
 * - NVLink fabric for scale-up (within node): 300 GB/s, 500ns
 * - Ethernet fabric for scale-out (between nodes): 100 Gbps, 10µs
 *
 * Example usage:
 * @code
 * HybridTopologyHelper helper;
 *
 * // Add NVLink fabric for node 0 (GPUs 0-3)
 * helper.AddNvLinkFabric(4, "ring", 0, "300Gbps", "500ns");
 *
 * // Add NVLink fabric for node 1 (GPUs 4-7)
 * helper.AddNvLinkFabric(4, "ring", 4, "300Gbps", "500ns");
 *
 * // Add Ethernet link between gateways
 * helper.AddCrossFabricLink(0, 1, 3, 4, "100Gbps", "10us");
 *
 * // Build the topology
 * NodeContainer allNodes = helper.Build();
 *
 * // Populate cross-fabric routing
 * helper.PopulateCrossFabricRouting();
 * @endcode
 */
class HybridTopologyHelper
{
  public:
    /**
     * @brief Constructor
     */
    HybridTopologyHelper();

    /**
     * @brief Destructor
     */
    ~HybridTopologyHelper();

    /**
     * @brief Add an NVLink fabric domain
     * @param numGpus Number of GPUs in this fabric
     * @param topologyType Topology type: "ring", "fullmesh", "switch"
     * @param baseRank Starting rank for GPUs in this fabric
     * @param dataRate Link data rate (default: "300Gbps")
     * @param delay Link delay (default: "500ns")
     * @return Fabric ID assigned to this fabric
     */
    uint16_t AddNvLinkFabric(uint32_t numGpus, std::string topologyType,
                             uint16_t baseRank,
                             std::string dataRate = "300Gbps",
                             std::string delay = "500ns");

    /**
     * @brief Add an Ethernet fabric domain
     * @param numGpus Number of GPUs in this fabric
     * @param topologyType Topology type: "ring", "fullmesh", "switch"
     * @param baseRank Starting rank for GPUs in this fabric
     * @param dataRate Link data rate (default: "100Gbps")
     * @param delay Link delay (default: "10us")
     * @return Fabric ID assigned to this fabric
     */
    uint16_t AddEthernetFabric(uint32_t numGpus, std::string topologyType,
                               uint16_t baseRank,
                               std::string dataRate = "100Gbps",
                               std::string delay = "10us");

    /**
     * @brief Set the gateway rank for a fabric
     * @param fabricId Fabric ID
     * @param gatewayRank Rank of the gateway node
     */
    void SetGatewayRank(uint16_t fabricId, uint16_t gatewayRank);

    /**
     * @brief Add a cross-fabric link between two gateways
     * @param fabric1Id First fabric ID
     * @param fabric2Id Second fabric ID
     * @param gateway1Rank Gateway rank in fabric 1
     * @param gateway2Rank Gateway rank in fabric 2
     * @param dataRate Link data rate (default: "100Gbps")
     * @param delay Link delay (default: "10us")
     */
    void AddCrossFabricLink(uint16_t fabric1Id, uint16_t fabric2Id,
                            uint16_t gateway1Rank, uint16_t gateway2Rank,
                            std::string dataRate = "100Gbps",
                            std::string delay = "10us");

    /**
     * @brief Set link degradation model for all links
     * @param model Link degradation model
     */
    void SetLinkDegradationModel(Ptr<LinkDegradationModel> model);

    /**
     * @brief Build the entire hybrid topology
     * @return Container of all nodes across all fabrics
     */
    NodeContainer Build();

    /**
     * @brief Populate cross-fabric routing tables
     *
     * This must be called after Build() to configure routing
     * for packets that need to traverse multiple fabrics.
     */
    void PopulateCrossFabricRouting();

    /**
     * @brief Get all nodes
     * @return Container of all nodes
     */
    NodeContainer GetAllNodes() const;

    /**
     * @brief Get nodes in a specific fabric
     * @param fabricId Fabric ID
     * @return Container of nodes in that fabric
     */
    NodeContainer GetFabricNodes(uint16_t fabricId) const;

    /**
     * @brief Get all endpoint applications
     * @return Container of all endpoints
     */
    ApplicationContainer GetAllEndpoints() const;

    /**
     * @brief Get endpoints in a specific fabric
     * @param fabricId Fabric ID
     * @return Container of endpoints in that fabric
     */
    ApplicationContainer GetFabricEndpoints(uint16_t fabricId) const;

    /**
     * @brief Get the fabric ID for a given rank
     * @param rank Endpoint rank
     * @return Fabric ID, or 0 if not found
     */
    uint16_t GetFabricIdForRank(uint16_t rank) const;

    /**
     * @brief Get the number of fabrics
     * @return Number of fabrics
     */
    uint32_t GetNFabrics() const;

    /**
     * @brief Get total number of GPUs across all fabrics
     * @return Total GPU count
     */
    uint32_t GetTotalNumGpus() const;

  private:
    /**
     * @brief Build a single fabric domain
     * @param config Fabric configuration
     */
    void BuildFabric(FabricConfig& config);

    /**
     * @brief Build cross-fabric links
     */
    void BuildCrossFabricLinks();

    /**
     * @brief Configure gateway nodes with cross-fabric routing
     */
    void ConfigureGateways();

    std::vector<FabricConfig> m_fabrics;           ///< Fabric configurations
    std::vector<CrossFabricLink> m_crossFabricLinks; ///< Cross-fabric links
    Ptr<LinkDegradationModel> m_linkDegradationModel; ///< Link degradation model
    NodeContainer m_allNodes;                      ///< All nodes
    ApplicationContainer m_allEndpoints;           ///< All endpoints
    std::unordered_map<uint16_t, uint16_t> m_rankToFabric; ///< rank -> fabricId mapping
    uint16_t m_nextFabricId;                       ///< Next fabric ID to assign
};

} // namespace ns3

#endif /* HYBRID_TOPOLOGY_HELPER_H */
