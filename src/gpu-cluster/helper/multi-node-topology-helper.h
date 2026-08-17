/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Helper for building multi-node GPU cluster topologies with RDMA interconnect
 */

#ifndef MULTI_NODE_TOPOLOGY_HELPER_H
#define MULTI_NODE_TOPOLOGY_HELPER_H

#include "ns3/net-device-container.h"
#include "ns3/node-container.h"
#include "ns3/application-container.h"
#include "ns3/fabric-type.h"
#include "ns3/nvswitch.h"
#include "ns3/link-degradation.h"

#include <string>
#include <vector>
#include <map>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Per-node configuration data
 */
struct NodeInfo
{
    uint32_t nodeId;
    uint16_t baseRank;
    NodeContainer gpuNodes;
    Ptr<Node> switchNode;
    ApplicationContainer endpoints;
    Ptr<NvSwitch> nvSwitch;
    std::vector<std::vector<Mac48Address>> gpuMacs;
    std::vector<std::vector<uint32_t>> gpuSwitchPorts;
    std::vector<Ptr<NetDevice>> interNodeDevices;
    std::vector<uint32_t> interNodeDeviceIndices;
    // RDMA routing: (remoteNodeId, localIdx) -> {deviceIdx, peerMac}
    // For GPU localIdx on this node, what device index and peer MAC to reach
    // same-local-rank GPU on remoteNodeId
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, Mac48Address>> rdmaRouteMap;
    // Fullmesh intra-node: localIdx -> logical device index mapping per endpoint
    // For GPU localIdx, what logical device index connects to peer at destLocalIdx
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> localP2pDevIdxMap;
};

/**
 * @ingroup gpu-cluster
 * @brief Helper to build multi-node GPU cluster topologies
 *
 * Each node is an NVSwitch domain (gpusPerNode GPUs connected via NVLink).
 * Inter-node links use RDMA/RoCE NICs on each GPU connecting to same-local-rank
 * GPUs on other nodes, matching real NCCL peer-to-peer inter-node patterns.
 *
 * Example: 2 nodes, 8 GPUs per node (16 total GPUs)
 * - Node 0: ranks 0-7, NVSwitch domain, NVLink4 18-lane links
 * - Node 1: ranks 8-15, NVSwitch domain, NVLink4 18-lane links
 * - Inter-node: GPU 0 connects to GPU 8 via RDMA NIC (400 Gb/s RoCE)
 *               GPU 1 connects to GPU 9 via RDMA NIC
 *               ... (8 RDMA links between the two nodes)
 */
class MultiNodeTopologyHelper
{
  public:
    MultiNodeTopologyHelper();
    ~MultiNodeTopologyHelper();

    // Intra-node configuration (applies to each node)
    void SetGpusPerNode(uint32_t gpus);
    void SetIntraNodeTopology(const std::string& topo); // "switched" (default), "fullmesh"
    void SetIntraNodeDataRate(const std::string& rate);
    void SetIntraNodeDelay(const std::string& delay);
    void SetIntraNodeFabricType(FabricType type);
    void SetLinksPerGpu(uint32_t links);
    void SetNumLanes(uint32_t lanes);
    void SetSprayChunkSize(uint32_t chunkSize);
    void SetVcCredits(uint32_t credits);
    void SetSwitchVoqDepth(uint32_t depth);
    void SetSwitchArbInterval(uint32_t intervalNs);
    void SetSwitchCutThroughDelay(uint64_t delayNs);
    /// Set the crossbar arbitration strategy on every built switch (default
    /// RoundRobinArbiter). Pass an ns3::Arbiter subclass to adapt a different
    /// arbitration method.
    void SetArbiter(Ptr<Arbiter> arbiter);

    // Inter-node configuration
    void SetNumNodes(uint32_t numNodes);
    void SetInterNodeDataRate(const std::string& rate);
    void SetInterNodeDelay(const std::string& delay);
    void SetInterNodeFabricType(FabricType type);
    void SetInterNodeTopology(const std::string& topo); // "fullmesh", "ring", "host"

    // Build
    NodeContainer Build();
    void PopulateInterNodeRouting();

    // Accessors
    ApplicationContainer GetAllEndpoints() const;
    NodeContainer GetAllNodes() const;
    NodeContainer GetGpuNodes() const;
    NodeContainer GetSwitchNodes() const;
    uint32_t GetNodeIdForRank(uint16_t rank) const;
    uint16_t GetLocalRankForGlobalRank(uint16_t globalRank) const;
    uint16_t GetGlobalRank(uint32_t nodeId, uint32_t localIdx) const;
    uint32_t GetTotalNumGpus() const;
    Ptr<NvSwitch> GetNvSwitch(uint32_t nodeId) const;

  private:
    void BuildIntraNode(uint32_t nodeId);
    void BuildIntraNodeFullmesh(uint32_t nodeId, NodeInfo& info);
    void BuildIntraNodeSwitched(uint32_t nodeId, NodeInfo& info);
    void BuildInterNodeLinks();

    uint32_t m_numNodes;
    uint32_t m_gpusPerNode;
    uint32_t m_linksPerGpu;
    uint32_t m_numLanes;
    uint32_t m_sprayChunkSize;
    uint32_t m_vcCredits;
    uint32_t m_switchVoqDepth;
    uint32_t m_switchArbIntervalNs;
    uint64_t m_switchCutThroughDelayNs;
    Ptr<Arbiter> m_arbiter;

    std::string m_intraNodeDataRate;
    std::string m_intraNodeDelay;
    FabricType m_intraNodeFabricType;
    std::string m_intraNodeTopology;

    std::string m_interNodeDataRate;
    std::string m_interNodeDelay;
    FabricType m_interNodeFabricType;
    std::string m_interNodeTopology;
    std::vector<Ptr<Node>> m_interNodeSwitchNodes;
    std::vector<Ptr<NvSwitch>> m_interNodeSwitches;

    std::vector<NodeInfo> m_nodeInfos;
    NodeContainer m_allGpuNodes;
    NodeContainer m_allSwitchNodes;
    NodeContainer m_allNodes;
    ApplicationContainer m_allEndpoints;
};

} // namespace ns3

#endif /* MULTI_NODE_TOPOLOGY_HELPER_H */
