/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Helper classes for GPU Cluster network configuration
 */

#ifndef GPU_CLUSTER_HELPER_H
#define GPU_CLUSTER_HELPER_H

#include "ns3/net-device-container.h"
#include "ns3/node-container.h"
#include "ns3/application-container.h"
#include "ns3/object-factory.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/nvswitch.h"
#include "ns3/link-degradation.h"
#include "ns3/contention-model.h"
#include "ns3/device-type.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ns3
{

/**
 * @ingroup gpu-cluster
 * @brief Helper to configure Fabric Endpoint applications
 */
class FabricEndpointHelper
{
  public:
    /**
     * @brief Constructor
     */
    FabricEndpointHelper();

    /**
     * @brief Set an attribute for the FabricEndpoint
     * @param name Attribute name
     * @param value Attribute value
     */
    void SetAttribute(std::string name, const AttributeValue& value);

    /**
     * @brief Install FabricEndpoint on a node
     * @param node The node to install on
     * @return The installed application
     */
    ApplicationContainer Install(Ptr<Node> node) const;

    /**
     * @brief Install FabricEndpoint on multiple nodes
     * @param nodes The nodes to install on
     * @return The installed applications
     */
    ApplicationContainer Install(NodeContainer nodes) const;

    /**
     * @brief Add a NetDevice to the endpoint after installation
     * @param app The FabricEndpoint application
     * @param device The NetDevice to add
     */
    void AddNetDevice(Ptr<Application> app, Ptr<NetDevice> device) const;

    /**
     * @brief Set the rank for the endpoint
     * @param app The FabricEndpoint application
     * @param rank The endpoint rank
     */
    void SetRank(Ptr<Application> app, uint16_t rank) const;

  private:
    /**
     * @brief Internal implementation of Install
     * @param node The node to install on
     * @return The installed application
     */
    Ptr<Application> InstallPriv(Ptr<Node> node) const;

    ObjectFactory m_factory; ///< Object factory for FabricEndpoint
};

// Backward compatibility
using GpuEndpointHelper = FabricEndpointHelper;

/**
 * @ingroup gpu-cluster
 * @brief Helper to configure NVSwitch devices
 */
class NvSwitchHelper
{
  public:
    /**
     * @brief Constructor
     */
    NvSwitchHelper();

    /**
     * @brief Set an attribute for the NvSwitch
     * @param name Attribute name
     * @param value Attribute value
     */
    void SetAttribute(std::string name, const AttributeValue& value);

    /**
     * @brief Set the switch implementation's TypeId.
     *
     * Default is "ns3::NvSwitch". Pass a different FabricSwitch subclass
     * TypeId to plug an alternative switch architecture (e.g. an AMD
     * Infinity Fabric switch, a Clos spine model, an OCS model). The
     * subclass must derive from FabricSwitch and register a TypeId.
     * @param typeId A FabricSwitch subclass TypeId string (e.g. "ns3::NvSwitch")
     */
    void SetSwitchType(const std::string& typeId);

    /**
     * @brief Install a switch on a node
     * @param node The node to install on
     * @return The installed NetDevice (switch device)
     */
    Ptr<NetDevice> Install(Ptr<Node> node) const;

    /**
     * @brief Add a port to the switch
     * @param switchDevice The switch device (a FabricSwitch)
     * @param portDevice The NetDevice to add as a port
     * @return Port number assigned
     */
    uint32_t AddPort(Ptr<NetDevice> switchDevice, Ptr<NetDevice> portDevice) const;

    /**
     * @brief Connect two switches
     * @param switch1 First switch device
     * @param switch2 Second switch device
     * @param channel The channel connecting them
     */
    void ConnectSwitches(Ptr<NetDevice> switch1, Ptr<NetDevice> switch2,
                         Ptr<Channel> channel) const;

  private:
    ObjectFactory m_factory; ///< Object factory for the switch (default ns3::NvSwitch)
};

/**
 * @ingroup gpu-cluster
 * @brief Helper to build GPU cluster topologies
 */
class GpuClusterTopologyHelper
{
  public:
    /**
     * @brief Constructor
     * @param numGpus Number of GPUs in the cluster
     * @param numSwitches Number of NVSwitches
     */
    GpuClusterTopologyHelper(uint32_t numGpus, uint32_t numSwitches);

    /**
     * @brief Set link data rate
     * @param rate Data rate string (e.g., "100Gbps")
     */
    void SetLinkDataRate(std::string rate);

    /**
     * @brief Set link delay
     * @param delay Delay string (e.g., "500ns")
     */
    void SetLinkDelay(std::string delay);

    /**
     * @brief Build a fully-connected topology (NVLink-like)
     * @return Container of GPU nodes
     */
    NodeContainer BuildFullyConnected();

    /**
     * @brief Build a fat-tree topology
     * @param radix Switch radix (ports per switch)
     * @return Container of all nodes (GPUs and switches)
     */
    NodeContainer BuildFatTree(uint32_t radix);

    /**
     * @brief Build a rail-optimized full fat tree.
     *
     * GPU rank r in every enclosure attaches to rail r.  Each rail leaf serves
     * a bounded number of enclosures. Small configurations connect every leaf
     * to every spine; larger configurations group leaves and spines behind a
     * core tier.
     * @param numRails GPUs per enclosure and number of rails
     * @param nodesPerLeaf Maximum enclosures served by one leaf on a rail
     * @param numSpineSwitches Number of shared spine switches
     * @param linksPerLeafSpine Parallel links between each leaf and spine
     * @param numCoreSwitches Number of core switches, or zero for two tiers
     * @return Container of all nodes (GPUs and fabric switches)
     */
    NodeContainer BuildRailOptimizedFatTree(uint32_t numRails,
                                            uint32_t nodesPerLeaf,
                                            uint32_t numSpineSwitches,
                                            uint32_t linksPerLeafSpine,
                                            uint32_t numCoreSwitches = 0);

    /**
     * @brief Build a ring topology (direct connect, no switches)
     * Each node has 2 NetDevices connected to left and right neighbors.
     * Routing tables are populated with shortest-path on ring.
     * @return Container of all nodes
     */
    NodeContainer BuildRing();

    /**
     * @brief Build a full-mesh topology (direct connect, no switches)
     * Each node has N-1 NetDevices, one per peer.
     * Routing tables map destRank -> deviceIndex.
     * @return Container of all nodes
     */
    NodeContainer BuildFullMesh();

    /**
     * @brief Build an NVL72 switch-plane topology (for Blackwell B200 NVL72)
     * Real NVL72 has 18 NVSwitch ASICs (9 trays × 2), each with 72 ports.
     * Each GPU has one NVLink to each ASIC, giving full non-blocking bisection BW.
     * @param numSwitchPlanes Number of NVSwitch ASIC planes (default 18 for NVL72)
     * @param gpusPerGroup GPUs per hierarchical group (default 12, = numGpus/6)
     * @return Container of all nodes (GPUs and switches)
     */
    NodeContainer BuildNvl72(uint32_t numSwitchPlanes = 18, uint32_t gpusPerGroup = 0);

    /**
     * @brief Build a leaf-spine topology (multi-switch, for NVL72-like supernodes)
     * @param numLeafSwitches Number of leaf switches (directly connect to GPUs)
     * @param numSpineSwitches Number of spine switches (interconnect leaf switches)
     * @return Container of all nodes (GPUs, leaf switches, spine switches)
     */
    NodeContainer BuildLeafSpine(uint32_t numLeafSwitches, uint32_t numSpineSwitches);

    /**
     * @brief Build a 3D torus topology (for TPU ICI-like interconnects)
     * @param dimX Number of nodes in X dimension
     * @param dimY Number of nodes in Y dimension
     * @param dimZ Number of nodes in Z dimension
     * @return Container of all nodes
     */
    NodeContainer BuildTorus(uint32_t dimX, uint32_t dimY, uint32_t dimZ);

    /**
     * @brief Build a 2D mesh topology (for UBMesh/HCCS-like interconnects)
     * @param rows Number of rows
     * @param cols Number of columns
     * @return Container of all nodes
     */
    NodeContainer BuildMesh2D(uint32_t rows, uint32_t cols);

    /**
     * @brief Build a hypercube topology (for AMD MI300X-like interconnects)
     * @param dimensions Number of hypercube dimensions (2^dimensions nodes)
     * @return Container of all nodes
     */
    NodeContainer BuildHypercube(uint32_t dimensions);

    /**
     * @brief Build a 2D full-mesh topology (for UB-Mesh rack)
     * node(r,c) connects to all nodes in row r and column c.
     * Each node has (rows-1)+(cols-1) links.
     * For 8x8 rack: 14 links per node, 64 nodes, 0 switch hops.
     * @param rows Number of rows in the 2D grid
     * @param cols Number of columns in the 2D grid
     * @return Container of all nodes
     */
    NodeContainer Build2DFullMesh(uint32_t rows, uint32_t cols);

    /**
     * @brief Build n-dimensional full-mesh topology (generalized UB-Mesh)
     * Within each 1D segment (all nodes sharing same values in other dimensions),
     * all nodes have direct pairwise connections.
     * @param dims Vector of dimension sizes (e.g., {8,8} for 2D, {4,4,4} for 3D)
     * @return Container of all nodes
     */
    NodeContainer BuildNDFullMesh(const std::vector<uint32_t>& dims);

    /**
     * @brief Build a 2D full-mesh + Clos hybrid topology (for UB SuperPod)
     * Intra-rack: 2D full-mesh direct connections (~100ns, 0 switch hops)
     * Inter-rack: spine switches connecting racks (~300-500ns, 1-2 switch hops)
     * @param rackRows Rows per rack (rack = rows*cols NPUs)
     * @param rackCols Columns per rack
     * @param numRacks Number of racks
     * @param numSpineSwitches Number of spine switches for inter-rack
     * @return Container of all nodes (NPUs and spine switches)
     */
    NodeContainer Build2DFullMeshClos(uint32_t rackRows, uint32_t rackCols,
                                       uint32_t numRacks, uint32_t numSpineSwitches);

    /**
     * @brief Build a 3-level hierarchical topology (NVSw + mid-tier + spine).
     *
     * Three switch tiers: leaf (GPU-facing), mid (inter-rack XCCL), spine
     * (inter-rack RoCE). GPUs connect to all leaf switches; each leaf
     * connects to all mid switches; each mid connects to all spine switches.
     * Models NVSwitch + NVLink-XCCL + RoCE disaggregation.
     * @param numLeafSwitches Leaf switches (GPU-facing)
     * @param numMidSwitches Mid-tier switches (XCCL)
     * @param numSpineSwitches Spine switches (RoCE)
     * @return Container of all nodes
     */
    NodeContainer Build3LevelHierarchical(uint32_t numLeafSwitches,
                                           uint32_t numMidSwitches,
                                           uint32_t numSpineSwitches);

    /**
     * @brief Build a Dragonfly+ topology.
     *
     * Each group is a fully-connected leaf-spine (all leaf switches in a
     * group connect to all spine switches in the same group). Groups are
     * interconnected by direct leaf-to-leaf "global" links in a full mesh
     * (every group pair has one global link). Models a Dragonfly+ fabric
     * with intra-group electrical and inter-group optical links.
     * @param numGroups Number of dragonfly groups
     * @param routersPerGroup Leaf switches per group (GPUs evenly divided)
     * @return Container of all nodes
     */
    NodeContainer BuildDragonflyPlus(uint32_t numGroups, uint32_t routersPerGroup);

    /**
     * @brief Build a multi-plane topology (independent switch planes).
     *
     * Each GPU connects to every plane with one link; each plane is a
     * single NvSwitch that all GPUs share. Distinct from BuildNvl72: planes
     * are independent L2 domains (no inter-plane links), and the number of
     * planes is a free parameter (not fixed at 18). Models multi-plane
     * fabrics where spraying is across planes rather than within one.
     * @param numPlanes Number of independent switch planes
     * @return Container of all nodes
     */
    NodeContainer BuildMultiPlane(uint32_t numPlanes);

    /**
     * @brief Install topology-aware ring/tree collective embedding on endpoints.
     *
     * Computes a locality-ordered rank permutation from the family geometry
     * (Gray code for hypercube, serpentine for torus/mesh, snake for 2D
     * full-mesh, NodeId-sorted for switched/hierarchical) and stores per-rank
     * ring next/prev + tree parent/children on each FabricEndpoint. Injectors
     * query these with a rank-arithmetic fallback when unset (no regression).
     * @param family Topology family key (hypercube|torus|mesh2d|2dfullmesh|
     *               ndfullmesh|leafspine|fattree|dragonflyplus|multiplane|
     *               3levelhierarchical|nvl72|railfattree|2dfullmeshclos|switched|
     *               ring|fullmesh)
     * @param dims Dimension vector for geometric families
     */
    void InstallCollectiveEmbedding(const std::string& family,
                                    const std::vector<uint32_t>& dims = {});

    /**
     * @brief Get GPU nodes
     * @return Container of GPU nodes
     */
    NodeContainer GetGpuNodes() const;

    /**
     * @brief Get switch nodes
     * @return Container of switch nodes
     */
    NodeContainer GetSwitchNodes() const;

    /**
     * @brief Get FabricEndpoint applications
     * @return Container of FabricEndpoint applications
     */
    ApplicationContainer GetEndpoints() const;

    /**
     * @brief Get GpuEndpoint applications (backward compat)
     * @return Container of endpoint applications
     */
    ApplicationContainer GetGpuEndpoints() const;

    /**
     * @brief Set device type for a specific rank (for heterogeneous clusters)
     * @param rank Device rank
     * @param type Device type
     */
    void SetDeviceType(uint16_t rank, DeviceType type);

    /**
     * @brief Set link degradation model to attach to all links during build
     * @param model Link degradation model (cloned for each link)
     */
    /**
     * @brief Set per-channel BER tiers for tier-aware error injection.
     *
     * When set (any value > 0), Build* functions pick the BER appropriate
     * for each channel's link class and medium instead of the single global
     * BER from m_linkDegradationModel. Zero-value tiers fall back to the
     * global BER.
     */
    void SetBerTiers(double berIntraNodeElectrical,
                     double berIntraRackElectrical,
                     double berInterRackElectrical,
                     double berInterRackOptical);

    /**
     * @brief Select the medium used by links between switch tiers.
     * @param medium "electrical" or "optical"
     */
    void SetInterSwitchMedium(const std::string& medium);

    /**
     * @brief Set link degradation model (shared, cloned per port during build)
     * @param model Degradation model
     */
    void SetLinkDegradationModel(Ptr<LinkDegradationModel> model);

    /**
     * @brief Set FEC model for NVSwitch port decode
     * NVLink FEC operates at link level: both GPU and NVSwitch ASIC endpoints
     * have FEC encode/decode. This propagates FEC to switch ports during build.
     * @param model FEC model (shared object, not cloned)
     */
    void SetFecModel(Ptr<FecModel> model);

    /**
     * @brief Restrict the configured FEC model to optical switch ports.
     */
    void SetFecOpticalOnly(bool opticalOnly);

    /**
     * @brief Enable LLR on NVSwitch ports (NACK on CRC/FEC failure at ingress)
     * @param enabled True to enable switch-side LLR
     */
    void SetSwitchLlrEnabled(bool enabled);

    /**
     * @brief Set contention model to attach to all endpoints during build
     * @param model Contention model (cloned for each endpoint)
     */
    void SetContentionModel(Ptr<ContentionModel> model);

    /**
     * @brief Set fabric ID for this topology
     * @param fabricId Fabric identifier (default 0)
     */
    void SetFabricId(uint16_t fabricId);

    /**
     * @brief Get fabric ID for this topology
     * @return Fabric identifier
     */
    uint16_t GetFabricId() const;

    /**
     * @brief Set fabric type for all endpoints in this topology
     * @param type Fabric type (NVLink, Ethernet, Hybrid)
     */
    void SetFabricType(FabricType type);

    /**
     * @brief Get fabric type for this topology
     * @return Fabric type
     */
    FabricType GetFabricType() const;

    /**
     * @brief Set number of links per GPU to switch (for switched topology)
     * @param links Number of parallel NVLinks per GPU
     */
    void SetLinksPerGpu(uint32_t links);

    /**
     * @brief Get number of links per GPU
     * @return Links per GPU
     */
    uint32_t GetLinksPerGpu() const;

    /**
     * @brief Set spray chunk size in bytes (for multi-link topologies)
     * @param bytes Spray chunk size
     */
    void SetSprayChunkSize(uint32_t bytes);

    /**
     * @brief Get spray chunk size
     * @return Spray chunk size in bytes
     */
    uint32_t GetSprayChunkSize() const;

    /**
     * @brief Set number of physical lanes per logical link
     * @param numLanes Number of lanes (1=no sub-lane spraying, 6=NVLink)
     */
    void SetNumLanes(uint32_t numLanes);

    /**
     * @brief Get number of physical lanes per logical link
     * @return Number of lanes
     */
    uint32_t GetNumLanes() const;

    /**
     * @brief Count physical optical links between fabric switches.
     *
     * A bidirectional point-to-point channel is counted once.
     */
    uint32_t GetOpticalInterSwitchLinkCount() const;

    /**
     * @brief Count optical inter-switch links that remain operational.
     */
    uint32_t GetOperationalOpticalInterSwitchLinkCount() const;

    /**
     * @brief Minimum BER among operational optical inter-switch links.
     * @return Zero when no operational optical link remains
     */
    double GetOperationalOpticalInterSwitchBerMin() const;

    /**
     * @brief Maximum BER among operational optical inter-switch links.
     * @return Zero when no operational optical link remains
     */
    double GetOperationalOpticalInterSwitchBerMax() const;

    /**
     * @brief Rebuild destination routes over all currently operational links.
     * @return True when every ordered GPU pair remains connected
     */
    bool RecomputeFailureAwareRoutes();

    /**
     * @brief Permanently remove one optical inter-switch link and rebuild routes.
     * @param index Stable index in the discovered optical-link list
     * @return True when the link exists and every reachable route was rebuilt
     */
    bool FailOpticalInterSwitchLink(uint32_t index);

    int32_t GetFailedOpticalLinkIndex() const;
    std::string GetFailedOpticalLinkDescription() const;
    double GetFailedOpticalLinkBer() const;
    uint64_t GetUnreachableGpuPairs() const;

  private:
    struct InterSwitchLink
    {
        Ptr<NvSwitch> leftSwitch;
        Ptr<NvSwitch> rightSwitch;
        uint32_t leftPort = 0;
        uint32_t rightPort = 0;
        uint32_t leftNodeId = 0;
        uint32_t rightNodeId = 0;
        std::string medium;
        double ber = 0.0;
    };

    std::vector<InterSwitchLink> DiscoverOpticalInterSwitchLinks() const;
    /**
     * @brief Create a per-port LinkDegradationModel with tier-appropriate BER
     * and LinkMetadata. Falls back to the shared model's BER when the tier
     * BER is unset (<= 0).
     * @param linkClass "intra_node" | "intra_rack" | "inter_rack"
     * @param medium "electrical" | "optical"
     * @param protocol "NVLink" | "RoCEv2" | "ICI" | "XCCL" | ...
     * @param bandwidthGbps Per-link bandwidth
     * @param distanceMeters Physical link distance (m)
     */
    Ptr<LinkDegradationModel> MakePortModel(
        const std::string& linkClass,
        const std::string& medium,
        const std::string& protocol,
        double bandwidthGbps,
        double distanceMeters) const;

    /**
     * @brief Create a cloned ContentionModel with bandwidth from m_dataRate
     * @return Cloned ContentionModel with weights copied and bandwidth set from link data rate
     */
    Ptr<ContentionModel> CreateContentionModelClone() const;

    /**
     * @brief Attach contention model to all endpoints in m_endpoints
     * Uses m_endpoints.GetN() to handle dimension-derived builders where
     * endpoint count differs from m_numGpus.
     */
    void AttachContentionModelToEndpoints();

    uint32_t m_numGpus;      ///< Number of GPUs
    uint32_t m_numSwitches;  ///< Number of switches
    std::string m_dataRate;  ///< Link data rate
    std::string m_delay;     ///< Link delay
    uint16_t m_fabricId;     ///< Fabric identifier
    uint8_t m_fabricType;    ///< Fabric type (FabricType value)
    uint32_t m_linksPerGpu;  ///< Links per GPU to switch (default 1)
    uint32_t m_sprayChunkSize; ///< Spray chunk size in bytes (default 131072)
    uint32_t m_numLanes;       ///< Number of physical lanes per logical link (default 1)

    std::unordered_map<uint16_t, DeviceType> m_deviceTypes; ///< rank -> device type overrides
    Ptr<LinkDegradationModel> m_linkDegradationModel; ///< Optional degradation model
    // Per-tier BER (Phase A.1). When > 0, overrides m_linkDegradationModel's
    // global BER for channels matching the tier. Zero = fall back to global.
    double m_berIntraNodeElectrical = 0.0;
    double m_berIntraRackElectrical = 0.0;
    double m_berInterRackElectrical = 0.0;
    double m_berInterRackOptical = 0.0;
    std::string m_interSwitchMedium = "electrical";
    Ptr<FecModel> m_fecModel; ///< Optional FEC model for switch port decode
    bool m_fecOpticalOnly = false;
    bool m_switchLlrEnabled = false; ///< LLR enabled on switch ports
    Ptr<ContentionModel> m_contentionModel; ///< Optional WFQ contention model

    int32_t m_failedOpticalLinkIndex = -1;
    std::string m_failedOpticalLinkDescription;
    double m_failedOpticalLinkBer = 0.0;
    uint64_t m_unreachableGpuPairs = 0;

    NodeContainer m_gpuNodes;      ///< GPU nodes
    NodeContainer m_switchNodes;   ///< Switch nodes
    ApplicationContainer m_endpoints; ///< GpuEndpoint applications
};

} // namespace ns3

#endif /* GPU_CLUSTER_HELPER_H */
