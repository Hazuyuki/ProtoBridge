/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Helper for building hybrid multi-fabric GPU cluster topologies
 */

#include "hybrid-topology-helper.h"
#include "gpu-cluster-helper.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/gateway-endpoint.h"
#include "ns3/hybrid-routing-table.h"
#include "ns3/log.h"
#include "ns3/string.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/mac48-address.h"

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("HybridTopologyHelper");

HybridTopologyHelper::HybridTopologyHelper()
    : m_nextFabricId(0)
{
    NS_LOG_FUNCTION(this);
}

HybridTopologyHelper::~HybridTopologyHelper()
{
    NS_LOG_FUNCTION(this);
}

uint16_t
HybridTopologyHelper::AddNvLinkFabric(uint32_t numGpus, std::string topologyType,
                                       uint16_t baseRank,
                                       std::string dataRate, std::string delay)
{
    NS_LOG_FUNCTION(this << numGpus << topologyType << baseRank);

    FabricConfig config;
    config.fabricId = m_nextFabricId++;
    config.fabricType = FabricType::NVLINK;
    config.numGpus = numGpus;
    config.topologyType = topologyType;
    config.baseRank = baseRank;
    config.dataRate = dataRate;
    config.delay = delay;

    m_fabrics.push_back(config);

    NS_LOG_DEBUG("Added NVLink fabric " << config.fabricId
                  << " with " << numGpus << " GPUs, baseRank=" << baseRank);

    return config.fabricId;
}

uint16_t
HybridTopologyHelper::AddEthernetFabric(uint32_t numGpus, std::string topologyType,
                                         uint16_t baseRank,
                                         std::string dataRate, std::string delay)
{
    NS_LOG_FUNCTION(this << numGpus << topologyType << baseRank);

    FabricConfig config;
    config.fabricId = m_nextFabricId++;
    config.fabricType = FabricType::ETHERNET;
    config.numGpus = numGpus;
    config.topologyType = topologyType;
    config.baseRank = baseRank;
    config.dataRate = dataRate;
    config.delay = delay;

    m_fabrics.push_back(config);

    NS_LOG_DEBUG("Added Ethernet fabric " << config.fabricId
                  << " with " << numGpus << " GPUs, baseRank=" << baseRank);

    return config.fabricId;
}

void
HybridTopologyHelper::SetGatewayRank(uint16_t fabricId, uint16_t gatewayRank)
{
    NS_LOG_FUNCTION(this << fabricId << gatewayRank);

    for (auto& fabric : m_fabrics)
    {
        if (fabric.fabricId == fabricId)
        {
            fabric.gatewayRank = gatewayRank;
            NS_LOG_DEBUG("Set gateway rank " << gatewayRank << " for fabric " << fabricId);
            return;
        }
    }
    NS_LOG_WARN("Fabric " << fabricId << " not found");
}

void
HybridTopologyHelper::AddCrossFabricLink(uint16_t fabric1Id, uint16_t fabric2Id,
                                          uint16_t gateway1Rank, uint16_t gateway2Rank,
                                          std::string dataRate, std::string delay)
{
    NS_LOG_FUNCTION(this << fabric1Id << fabric2Id << gateway1Rank << gateway2Rank);

    CrossFabricLink link;
    link.fabric1Id = fabric1Id;
    link.fabric2Id = fabric2Id;
    link.gateway1Rank = gateway1Rank;
    link.gateway2Rank = gateway2Rank;
    link.dataRate = dataRate;
    link.delay = delay;

    m_crossFabricLinks.push_back(link);

    // Set gateway ranks in fabric configs
    SetGatewayRank(fabric1Id, gateway1Rank);
    SetGatewayRank(fabric2Id, gateway2Rank);
}

void
HybridTopologyHelper::SetLinkDegradationModel(Ptr<LinkDegradationModel> model)
{
    m_linkDegradationModel = model;
}

NodeContainer
HybridTopologyHelper::Build()
{
    NS_LOG_FUNCTION(this);

    // Build each fabric
    for (auto& fabric : m_fabrics)
    {
        BuildFabric(fabric);
    }

    // Build cross-fabric links
    BuildCrossFabricLinks();

    // Configure gateways with cross-fabric routing
    ConfigureGateways();

    return m_allNodes;
}

void
HybridTopologyHelper::BuildFabric(FabricConfig& config)
{
    NS_LOG_FUNCTION(this << config.fabricId);

    // Create GPU nodes for this fabric
    config.nodes.Create(config.numGpus);

    // Add to all nodes
    m_allNodes.Add(config.nodes);

    // Create endpoints
    FabricEndpointHelper endpointHelper;
    config.endpoints = endpointHelper.Install(config.nodes);
    m_allEndpoints.Add(config.endpoints);

    // Create point-to-point helper
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(config.dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(config.delay));

    // Build topology based on type
    if (config.topologyType == "ring")
    {
        // Ring topology: each node connects to two neighbors
        for (uint32_t i = 0; i < config.numGpus; ++i)
        {
            uint16_t rank = config.baseRank + i;
            uint32_t nextIdx = (i + 1) % config.numGpus;
            uint32_t prevIdx = (i + config.numGpus - 1) % config.numGpus;
            (void)prevIdx;

            // Set rank and fabric type
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(config.endpoints.Get(i));
            ep->SetRank(rank);
            ep->SetFabricType(config.fabricType);

            // Map rank to fabric
            m_rankToFabric[rank] = config.fabricId;

            // Connect to next neighbor (if not already connected)
            if (i < config.numGpus - 1 || config.numGpus == 2)
            {
                // Check if this connection already exists
                uint16_t nextRank = config.baseRank + nextIdx;

                // Connect node i to node nextIdx
                NetDeviceContainer devices = p2p.Install(config.nodes.Get(i), config.nodes.Get(nextIdx));

                ep->AddNetDevice(devices.Get(0));
                ep->SetNeighborMac(nextRank, Mac48Address::ConvertFrom(devices.Get(1)->GetAddress()));

                // Add reverse device to next neighbor
                Ptr<FabricEndpoint> nextEp = DynamicCast<FabricEndpoint>(config.endpoints.Get(nextIdx));
                nextEp->AddNetDevice(devices.Get(1));
                nextEp->SetNeighborMac(rank, Mac48Address::ConvertFrom(devices.Get(0)->GetAddress()));
            }
        }

        // Set up routing table for ring
        for (uint32_t i = 0; i < config.numGpus; ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(config.endpoints.Get(i));
            uint16_t myRank = config.baseRank + i;
            (void)myRank;

            for (uint32_t j = 0; j < config.numGpus; ++j)
            {
                if (i == j) continue;

                uint16_t destRank = config.baseRank + j;

                // Find shortest path on ring
                int32_t distForward = (j > i) ? (j - i) : (config.numGpus - i + j);
                int32_t distBackward = (i > j) ? (i - j) : (config.numGpus - j + i);

                // Use device 0 for forward (to next), device 1 for backward (to prev)
                // But check how many devices we have
                uint32_t deviceIdx = (distForward <= distBackward) ? 0 : 1;
                if (ep->GetNNetDevices() > deviceIdx)
                {
                    ep->SetRoutingEntry(destRank, deviceIdx);
                }
            }
        }
    }
    else if (config.topologyType == "fullmesh")
    {
        // Full mesh: each node connects to all others
        for (uint32_t i = 0; i < config.numGpus; ++i)
        {
            uint16_t rank = config.baseRank + i;
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(config.endpoints.Get(i));
            ep->SetRank(rank);
            ep->SetFabricType(config.fabricType);

            m_rankToFabric[rank] = config.fabricId;

            // Connect to all higher-indexed nodes
            for (uint32_t j = i + 1; j < config.numGpus; ++j)
            {
                uint16_t peerRank = config.baseRank + j;

                NetDeviceContainer devices = p2p.Install(config.nodes.Get(i), config.nodes.Get(j));

                // Add device to both endpoints
                ep->AddNetDevice(devices.Get(0));
                ep->SetNeighborMac(peerRank, Mac48Address::ConvertFrom(devices.Get(1)->GetAddress()));

                Ptr<FabricEndpoint> peerEp = DynamicCast<FabricEndpoint>(config.endpoints.Get(j));
                peerEp->AddNetDevice(devices.Get(1));
                peerEp->SetNeighborMac(rank, Mac48Address::ConvertFrom(devices.Get(0)->GetAddress()));
            }

            // Set up routing: each destination has its own device
            for (uint32_t j = 0; j < config.numGpus; ++j)
            {
                if (i == j) continue;
                uint16_t destRank = config.baseRank + j;
                uint32_t deviceIdx = (j < i) ? j : (j - 1);
                ep->SetRoutingEntry(destRank, deviceIdx);
            }
        }
    }
    else
    {
        // Default: switch-based topology
        NS_LOG_DEBUG("Building switch-based topology for fabric " << config.fabricId);

        // Create a switch node
        NodeContainer switchNodes;
        switchNodes.Create(1);
        m_allNodes.Add(switchNodes);

        NvSwitchHelper switchHelper;
        Ptr<NetDevice> sw = switchHelper.Install(switchNodes.Get(0));

        // Connect each GPU to the switch
        for (uint32_t i = 0; i < config.numGpus; ++i)
        {
            uint16_t rank = config.baseRank + i;

            NetDeviceContainer devices = p2p.Install(config.nodes.Get(i), switchNodes.Get(0));

            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(config.endpoints.Get(i));
            ep->SetRank(rank);
            ep->SetFabricType(config.fabricType);
            ep->AddNetDevice(devices.Get(0));

            m_rankToFabric[rank] = config.fabricId;

            switchHelper.AddPort(sw, devices.Get(1));
        }
    }

    NS_LOG_DEBUG("Built fabric " << config.fabricId << " with "
                  << config.numGpus << " GPUs, topology=" << config.topologyType);
}

void
HybridTopologyHelper::BuildCrossFabricLinks()
{
    NS_LOG_FUNCTION(this);

    PointToPointHelper p2p;

    for (const auto& link : m_crossFabricLinks)
    {
        // Find the gateway nodes in each fabric
        Ptr<Node> gateway1 = nullptr;
        Ptr<Node> gateway2 = nullptr;
        Ptr<FabricEndpoint> ep1 = nullptr;
        Ptr<FabricEndpoint> ep2 = nullptr;

        // Find gateway 1
        for (const auto& fabric : m_fabrics)
        {
            if (fabric.fabricId == link.fabric1Id)
            {
                uint32_t localIdx = link.gateway1Rank - fabric.baseRank;
                if (localIdx < fabric.numGpus)
                {
                    gateway1 = fabric.nodes.Get(localIdx);
                    ep1 = DynamicCast<FabricEndpoint>(fabric.endpoints.Get(localIdx));
                }
                break;
            }
        }

        // Find gateway 2
        for (const auto& fabric : m_fabrics)
        {
            if (fabric.fabricId == link.fabric2Id)
            {
                uint32_t localIdx = link.gateway2Rank - fabric.baseRank;
                if (localIdx < fabric.numGpus)
                {
                    gateway2 = fabric.nodes.Get(localIdx);
                    ep2 = DynamicCast<FabricEndpoint>(fabric.endpoints.Get(localIdx));
                }
                break;
            }
        }

        if (!gateway1 || !gateway2 || !ep1 || !ep2)
        {
            NS_LOG_ERROR("Could not find gateway nodes for cross-fabric link");
            continue;
        }

        // Configure point-to-point link
        p2p.SetDeviceAttribute("DataRate", StringValue(link.dataRate));
        p2p.SetChannelAttribute("Delay", StringValue(link.delay));

        // Install link
        NetDeviceContainer devices = p2p.Install(gateway1, gateway2);

        // Add Ethernet device to each gateway
        // The device index will be higher than the NVLink devices
        ep1->AddNetDevice(devices.Get(0));
        ep1->SetNeighborMac(link.gateway2Rank, Mac48Address::ConvertFrom(devices.Get(1)->GetAddress()));

        ep2->AddNetDevice(devices.Get(1));
        ep2->SetNeighborMac(link.gateway1Rank, Mac48Address::ConvertFrom(devices.Get(0)->GetAddress()));

        NS_LOG_DEBUG("Built cross-fabric link: fabric " << link.fabric1Id
                      << " rank " << link.gateway1Rank
                      << " <-> fabric " << link.fabric2Id
                      << " rank " << link.gateway2Rank);
    }
}

void
HybridTopologyHelper::ConfigureGateways()
{
    NS_LOG_FUNCTION(this);

    // For each gateway, we need to set up cross-fabric routing
    // This is done after PopulateCrossFabricRouting() is called
}

void
HybridTopologyHelper::PopulateCrossFabricRouting()
{
    NS_LOG_FUNCTION(this);

    // For each cross-fabric link, configure routing on all nodes
    for (const auto& link : m_crossFabricLinks)
    {
        // Find fabrics
        const FabricConfig* fabric1 = nullptr;
        const FabricConfig* fabric2 = nullptr;

        for (const auto& fabric : m_fabrics)
        {
            if (fabric.fabricId == link.fabric1Id) fabric1 = &fabric;
            if (fabric.fabricId == link.fabric2Id) fabric2 = &fabric;
        }

        if (!fabric1 || !fabric2)
        {
            NS_LOG_ERROR("Could not find fabrics for cross-fabric routing");
            continue;
        }

        // Get gateway endpoints
        uint32_t gw1LocalIdx = link.gateway1Rank - fabric1->baseRank;
        uint32_t gw2LocalIdx = link.gateway2Rank - fabric2->baseRank;

        Ptr<FabricEndpoint> gw1Ep = DynamicCast<FabricEndpoint>(fabric1->endpoints.Get(gw1LocalIdx));
        Ptr<FabricEndpoint> gw2Ep = DynamicCast<FabricEndpoint>(fabric2->endpoints.Get(gw2LocalIdx));

        if (!gw1Ep || !gw2Ep)
        {
            NS_LOG_ERROR("Could not find gateway endpoints");
            continue;
        }

        // The gateway's Ethernet device should be the last device added
        uint32_t gw1EthDevIdx = gw1Ep->GetNNetDevices() - 1;
        uint32_t gw2EthDevIdx = gw2Ep->GetNNetDevices() - 1;

        // Configure gateway 1: route to all ranks in fabric 2 via Ethernet
        for (uint32_t i = 0; i < fabric2->numGpus; ++i)
        {
            uint16_t destRank = fabric2->baseRank + i;
            // Route to destRank via gateway 2 (use Ethernet device)
            gw1Ep->SetRoutingEntry(destRank, gw1EthDevIdx);
        }

        // Configure gateway 2: route to all ranks in fabric 1 via Ethernet
        for (uint32_t i = 0; i < fabric1->numGpus; ++i)
        {
            uint16_t destRank = fabric1->baseRank + i;
            // Route to destRank via gateway 1 (use Ethernet device)
            gw2Ep->SetRoutingEntry(destRank, gw2EthDevIdx);
        }

        // Configure all non-gateway nodes in fabric 1 to route to fabric 2 via gateway 1
        for (uint32_t i = 0; i < fabric1->numGpus; ++i)
        {
            uint16_t rank = fabric1->baseRank + i;
            if (rank == link.gateway1Rank) continue;  // Skip gateway itself

            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(fabric1->endpoints.Get(i));
            if (!ep) continue;

            // Find the device index that connects to the gateway
            uint32_t gwDevIdx = ep->GetRoutingDeviceIndex(link.gateway1Rank);

            // Route all fabric 2 destinations via gateway
            for (uint32_t j = 0; j < fabric2->numGpus; ++j)
            {
                uint16_t destRank = fabric2->baseRank + j;
                ep->SetRoutingEntry(destRank, gwDevIdx);
                ep->SetNeighborMac(destRank, gw1Ep->GetNeighborMac(link.gateway2Rank));
            }
        }

        // Configure all non-gateway nodes in fabric 2 to route to fabric 1 via gateway 2
        for (uint32_t i = 0; i < fabric2->numGpus; ++i)
        {
            uint16_t rank = fabric2->baseRank + i;
            if (rank == link.gateway2Rank) continue;  // Skip gateway itself

            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(fabric2->endpoints.Get(i));
            if (!ep) continue;

            // Find the device index that connects to the gateway
            uint32_t gwDevIdx = ep->GetRoutingDeviceIndex(link.gateway2Rank);

            // Route all fabric 1 destinations via gateway
            for (uint32_t j = 0; j < fabric1->numGpus; ++j)
            {
                uint16_t destRank = fabric1->baseRank + j;
                ep->SetRoutingEntry(destRank, gwDevIdx);
                ep->SetNeighborMac(destRank, gw2Ep->GetNeighborMac(link.gateway1Rank));
            }
        }

        NS_LOG_DEBUG("Configured cross-fabric routing between fabric "
                     << link.fabric1Id << " and " << link.fabric2Id);
    }
}

NodeContainer
HybridTopologyHelper::GetAllNodes() const
{
    return m_allNodes;
}

NodeContainer
HybridTopologyHelper::GetFabricNodes(uint16_t fabricId) const
{
    for (const auto& fabric : m_fabrics)
    {
        if (fabric.fabricId == fabricId)
        {
            return fabric.nodes;
        }
    }
    return NodeContainer();
}

ApplicationContainer
HybridTopologyHelper::GetAllEndpoints() const
{
    return m_allEndpoints;
}

ApplicationContainer
HybridTopologyHelper::GetFabricEndpoints(uint16_t fabricId) const
{
    for (const auto& fabric : m_fabrics)
    {
        if (fabric.fabricId == fabricId)
        {
            return fabric.endpoints;
        }
    }
    return ApplicationContainer();
}

uint16_t
HybridTopologyHelper::GetFabricIdForRank(uint16_t rank) const
{
    auto it = m_rankToFabric.find(rank);
    if (it != m_rankToFabric.end())
    {
        return it->second;
    }
    return 0;
}

uint32_t
HybridTopologyHelper::GetNFabrics() const
{
    return static_cast<uint32_t>(m_fabrics.size());
}

uint32_t
HybridTopologyHelper::GetTotalNumGpus() const
{
    uint32_t total = 0;
    for (const auto& fabric : m_fabrics)
    {
        total += fabric.numGpus;
    }
    return total;
}

} // namespace ns3
