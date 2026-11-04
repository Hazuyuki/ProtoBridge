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
    // "fullmesh", "ring", "host", "tor", "rail", "leafspine", "torus2d",
    // "dragonfly"
    void SetInterNodeTopology(const std::string& topo);
    /// leafspine: number of spine switches (0 = auto: gpusPerNode, which is
    /// non-blocking at equal link rates -- each leaf has gpusPerNode down
    /// links and one up link per spine).
    void SetInterNodeSpines(uint32_t spines);
    /// torus2d: grid width W (height = numNodes / W). 0 = auto: the largest
    /// divisor of numNodes that is <= sqrt(numNodes).
    void SetInterNodeTorusWidth(uint32_t width);
    /// dragonfly: nodes per group (0 = auto: the divisor of numNodes closest
    /// to sqrt(numNodes)).
    void SetInterNodeDragonflyGroupSize(uint32_t groupSize);
    // Switch TypeId for the inter-node (So) fabric's switched topologies (e.g.
    // "host"). Default "ns3::NvSwitch"; set to "ns3::EthernetSwitch" for a real
    // Ethernet RoCEv2 So fabric (EthernetSwitch derives from NvSwitch, so the
    // existing datapath wiring is reused unchanged).
    void SetInterNodeSwitchType(const std::string& typeId);
    /// Enable LLR (link-level retry) on every inter-node (So) switch so the
    /// lossy EthernetSwitch can emit NACKs back to senders on VOQ tail-drop
    /// (EmitNackBack gates on the switch's m_llrEnabled). Default false keeps
    /// the lossless RoCE path byte-identical (RoCE avoids drops via ECN, so it
    /// never relies on switch-emitted NACKs); set true only for the plain
    /// (lossy) Ethernet path.
    void SetInterNodeSwitchLlrEnabled(bool enabled);
    /// PFC (802.1Qbb-style) on every inter-node (So) switch. mode 0 = off
    /// (default, byte-identical legacy), 1 = static pause threshold, 2 =
    /// dynamic (HPCC SwitchMmu-style headroom-scaled), 3 = quanta
    /// (802.1Qbb pause-time semantics: PAUSE carries a pause time, the
    /// endpoint auto-expires, the switch re-asserts at pausetime/2).
    /// Threshold/offset/pausetime ride the EthernetSwitch attributes
    /// (Config::SetDefault before Build, or the setters below for per-helper
    /// overrides).
    void SetInterNodeSwitchPfcMode(uint32_t mode);
    void SetInterNodeSwitchPfcPauseThreshold(uint32_t threshold);
    void SetInterNodeSwitchPfcResumeOffset(uint32_t offset);
    void SetInterNodeSwitchPfcPauseTime(uint32_t pauseTimeUs);

  private:
    /// Apply the PFC attributes to an So switch (no-op for mode 0 and for
    /// non-EthernetSwitch subtypes via SetAttributeFailSafe).
    void ApplyPfcAttributes(Ptr<NvSwitch> sw) const;

  public:

    // Build
    NodeContainer Build();
    void PopulateInterNodeRouting();

    // Accessors
    ApplicationContainer GetAllEndpoints() const;
    uint16_t GetGlobalRank(uint32_t nodeId, uint32_t localIdx) const;
    Ptr<NvSwitch> GetNvSwitch(uint32_t nodeId) const;

  private:
    void BuildIntraNode(uint32_t nodeId);
    void BuildIntraNodeFullmesh(uint32_t nodeId, NodeInfo& info);
    void BuildIntraNodeSwitched(uint32_t nodeId, NodeInfo& info);
    void BuildInterNodeLinks();
    /// Create + configure one inter-node switch (VOQ depth, arbitration,
    /// cut-through, arbiter, LLR arming) of type m_interNodeSwitchType and
    /// register its node in m_allSwitchNodes / m_allNodes. Returns the switch;
    /// switchNode receives the hosting node.
    Ptr<NvSwitch> CreateInterNodeSwitch(Ptr<Node>& switchNode);
    /// Gateway-topology (torus2d / dragonfly) node-level next hop from
    /// nodeId toward dstNode (both != each other; returns a direct neighbor
    /// of nodeId).
    uint32_t GatewayNextHop(uint32_t nodeId, uint32_t dstNode) const;
    uint32_t TorusWidth() const;     ///< resolved torus2d width (auto if 0)
    uint32_t DragonflyGroupSize() const; ///< resolved dragonfly group size (auto if 0)

    uint32_t m_numNodes;
    uint32_t m_gpusPerNode;
    uint32_t m_linksPerGpu;
    uint32_t m_numLanes;
    uint32_t m_sprayChunkSize;
    uint32_t m_vcCredits;
    uint32_t m_switchVoqDepth;
    uint32_t m_switchArbIntervalNs;
    uint64_t m_switchCutThroughDelayNs;
    /// PFC attributes applied to every inter-node (So) switch (mode 0 = off).
    uint32_t m_switchPfcMode;
    uint32_t m_switchPfcPauseThreshold;
    uint32_t m_switchPfcResumeOffset;
    uint32_t m_switchPfcPauseTimeUs;
    Ptr<Arbiter> m_arbiter;

    std::string m_intraNodeDataRate;
    std::string m_intraNodeDelay;
    FabricType m_intraNodeFabricType;
    std::string m_intraNodeTopology;

    std::string m_interNodeDataRate;
    std::string m_interNodeDelay;
    FabricType m_interNodeFabricType;
    std::string m_interNodeTopology;
    std::string m_interNodeSwitchType{"ns3::NvSwitch"}; ///< So switch TypeId (NvSwitch default; EthernetSwitch for RoCE)
    bool m_interNodeSwitchLlrEnabled = false;            ///< LLR on So switches (default false = RoCE parity)
    uint32_t m_interNodeSpines = 0;             ///< leafspine spine count (0 = auto)
    uint32_t m_interNodeTorusWidth = 0;         ///< torus2d width (0 = auto)
    uint32_t m_interNodeDragonflyGroupSize = 0; ///< dragonfly group size (0 = auto)

    std::vector<NodeInfo> m_nodeInfos;
    NodeContainer m_allGpuNodes;
    NodeContainer m_allSwitchNodes;
    NodeContainer m_allNodes;
    ApplicationContainer m_allEndpoints;
};

} // namespace ns3

#endif /* MULTI_NODE_TOPOLOGY_HELPER_H */
