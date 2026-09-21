/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Multi-node GPU cluster topology builder implementation
 */

#include "multi-node-topology-helper.h"
#include "gpu-cluster-helper.h"  // for FabricEndpointHelper, NvSwitchHelper
#include "ns3/fabric-endpoint.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/data-rate.h"
#include "ns3/log.h"
#include "ns3/mac48-address.h"
#include "ns3/drop-tail-queue.h"

#include <cmath>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("MultiNodeTopologyHelper");

MultiNodeTopologyHelper::MultiNodeTopologyHelper()
    : m_numNodes(1),
      m_gpusPerNode(8),
      m_linksPerGpu(18),
      m_numLanes(1),
      m_sprayChunkSize(131072),
      m_vcCredits(104857600),
      m_switchVoqDepth(10000),
      m_switchArbIntervalNs(100),
      m_switchCutThroughDelayNs(200),
      m_arbiter(nullptr),
      m_intraNodeDataRate("3600Gbps"),
      m_intraNodeDelay("400ns"),
      m_intraNodeFabricType(FabricType::NVLINK),
      m_intraNodeTopology("switched"),
      m_interNodeDataRate("400Gbps"),
      m_interNodeDelay("10us"),
      m_interNodeFabricType(FabricType::ROCE),
      m_interNodeTopology("fullmesh"),
      m_interNodeSwitchLlrEnabled(false)
{}

MultiNodeTopologyHelper::~MultiNodeTopologyHelper()
{}

void MultiNodeTopologyHelper::SetGpusPerNode(uint32_t gpus) { m_gpusPerNode = gpus; }
void MultiNodeTopologyHelper::SetIntraNodeTopology(const std::string& topo) { m_intraNodeTopology = topo; }
void MultiNodeTopologyHelper::SetIntraNodeDataRate(const std::string& rate) { m_intraNodeDataRate = rate; }
void MultiNodeTopologyHelper::SetIntraNodeDelay(const std::string& delay) { m_intraNodeDelay = delay; }
void MultiNodeTopologyHelper::SetIntraNodeFabricType(FabricType type) { m_intraNodeFabricType = type; }
void MultiNodeTopologyHelper::SetLinksPerGpu(uint32_t links) { m_linksPerGpu = links; }
void MultiNodeTopologyHelper::SetNumLanes(uint32_t lanes) { m_numLanes = lanes; }
void MultiNodeTopologyHelper::SetSprayChunkSize(uint32_t chunkSize) { m_sprayChunkSize = chunkSize; }
void MultiNodeTopologyHelper::SetVcCredits(uint32_t credits) { m_vcCredits = credits; }
void MultiNodeTopologyHelper::SetSwitchVoqDepth(uint32_t depth) { m_switchVoqDepth = depth; }
void MultiNodeTopologyHelper::SetSwitchArbInterval(uint32_t intervalNs) { m_switchArbIntervalNs = intervalNs; }
void MultiNodeTopologyHelper::SetSwitchCutThroughDelay(uint64_t delayNs) { m_switchCutThroughDelayNs = delayNs; }
void MultiNodeTopologyHelper::SetArbiter(Ptr<Arbiter> arbiter) { m_arbiter = arbiter; }
void MultiNodeTopologyHelper::SetNumNodes(uint32_t numNodes) { m_numNodes = numNodes; }
void MultiNodeTopologyHelper::SetInterNodeDataRate(const std::string& rate) { m_interNodeDataRate = rate; }
void MultiNodeTopologyHelper::SetInterNodeDelay(const std::string& delay) { m_interNodeDelay = delay; }
void MultiNodeTopologyHelper::SetInterNodeFabricType(FabricType type) { m_interNodeFabricType = type; }
void MultiNodeTopologyHelper::SetInterNodeTopology(const std::string& topo) { m_interNodeTopology = topo; }
void MultiNodeTopologyHelper::SetInterNodeSwitchType(const std::string& typeId) { m_interNodeSwitchType = typeId; }
void MultiNodeTopologyHelper::SetInterNodeSwitchLlrEnabled(bool enabled) { m_interNodeSwitchLlrEnabled = enabled; }
void MultiNodeTopologyHelper::SetInterNodeSpines(uint32_t spines) { m_interNodeSpines = spines; }
void MultiNodeTopologyHelper::SetInterNodeTorusWidth(uint32_t width) { m_interNodeTorusWidth = width; }
void MultiNodeTopologyHelper::SetInterNodeDragonflyGroupSize(uint32_t groupSize) { m_interNodeDragonflyGroupSize = groupSize; }

Ptr<NvSwitch>
MultiNodeTopologyHelper::CreateInterNodeSwitch(Ptr<Node>& switchNode)
{
    NvSwitchHelper switchHelper;
    switchHelper.SetSwitchType(m_interNodeSwitchType);
    switchNode = CreateObject<Node>();
    Ptr<NetDevice> switchDevice = switchHelper.Install(switchNode);
    Ptr<NvSwitch> sw = DynamicCast<NvSwitch>(switchDevice);
    NS_ASSERT(sw);
    sw->SetVoqDepth(m_switchVoqDepth);
    sw->SetArbitrationInterval(m_switchArbIntervalNs);
    if (m_switchCutThroughDelayNs > 0)
    {
        sw->SetCutThroughDelay(m_switchCutThroughDelayNs);
    }
    if (m_arbiter)
    {
        sw->SetArbiter(m_arbiter);
    }
    sw->SetLlrEnabled(m_interNodeSwitchLlrEnabled);
    m_allSwitchNodes.Add(switchNode);
    m_allNodes.Add(switchNode);
    return sw;
}

uint32_t
MultiNodeTopologyHelper::TorusWidth() const
{
    if (m_interNodeTorusWidth > 0)
    {
        NS_ABORT_MSG_IF(m_numNodes % m_interNodeTorusWidth != 0,
                        "torus2d: numNodes=" << m_numNodes
                        << " is not divisible by width=" << m_interNodeTorusWidth);
        return m_interNodeTorusWidth;
    }
    // Auto: largest divisor of numNodes that is <= sqrt(numNodes)
    uint32_t width = 1;
    for (uint32_t w = 1; w * w <= m_numNodes; ++w)
    {
        if (m_numNodes % w == 0)
        {
            width = w;
        }
    }
    return width;
}

uint32_t
MultiNodeTopologyHelper::DragonflyGroupSize() const
{
    if (m_interNodeDragonflyGroupSize > 0)
    {
        NS_ABORT_MSG_IF(m_numNodes % m_interNodeDragonflyGroupSize != 0,
                        "dragonfly: numNodes=" << m_numNodes
                        << " is not divisible by groupSize=" << m_interNodeDragonflyGroupSize);
        return m_interNodeDragonflyGroupSize;
    }
    // Auto: divisor of numNodes closest to sqrt(numNodes) (ties -> smaller)
    uint32_t best = 1;
    for (uint32_t g = 1; g <= m_numNodes; ++g)
    {
        if (m_numNodes % g != 0)
        {
            continue;
        }
        double target = std::sqrt(static_cast<double>(m_numNodes));
        if (std::abs(static_cast<double>(g) - target) <
            std::abs(static_cast<double>(best) - target))
        {
            best = g;
        }
    }
    return best;
}

uint32_t
MultiNodeTopologyHelper::GatewayNextHop(uint32_t nodeId, uint32_t dstNode) const
{
    NS_ASSERT(nodeId != dstNode);
    if (m_interNodeTopology == "torus2d")
    {
        // Dimension-order (x then y) shortest-wrap routing
        uint32_t width = TorusWidth();
        uint32_t height = m_numNodes / width;
        uint32_t x = nodeId % width;
        uint32_t y = nodeId / width;
        uint32_t dx = dstNode % width;
        uint32_t dy = dstNode / width;
        if (x != dx)
        {
            uint32_t forwardDist = (dx - x + width) % width;
            uint32_t backwardDist = (x - dx + width) % width;
            uint32_t nx = forwardDist <= backwardDist ? (x + 1) % width
                                                      : (x + width - 1) % width;
            return y * width + nx;
        }
        uint32_t forwardDist = (dy - y + height) % height;
        uint32_t backwardDist = (y - dy + height) % height;
        uint32_t ny = forwardDist <= backwardDist ? (y + 1) % height
                                                  : (y + height - 1) % height;
        return ny * width + x;
    }
    // dragonfly: minimal l-g-l routing
    uint32_t groupSize = DragonflyGroupSize();
    uint32_t srcGroup = nodeId / groupSize;
    uint32_t dstGroup = dstNode / groupSize;
    if (srcGroup == dstGroup)
    {
        return dstNode; // direct intra-group gateway link
    }
    // The node in srcGroup holding the global link to dstGroup (palmtree
    // assignment, mirrored in BuildInterNodeLinks)
    uint32_t holderIdx = (dstGroup - (dstGroup > srcGroup ? 1 : 0)) % groupSize;
    uint32_t holder = srcGroup * groupSize + holderIdx;
    if (nodeId != holder)
    {
        return holder; // intra-group hop toward the global-link holder
    }
    // Cross the global link to dstGroup's holder of the link back to srcGroup
    uint32_t peerIdx = (srcGroup - (srcGroup > dstGroup ? 1 : 0)) % groupSize;
    return dstGroup * groupSize + peerIdx;
}

uint16_t MultiNodeTopologyHelper::GetGlobalRank(uint32_t nodeId, uint32_t localIdx) const
{
    return static_cast<uint16_t>(nodeId * m_gpusPerNode + localIdx);
}
Ptr<NvSwitch> MultiNodeTopologyHelper::GetNvSwitch(uint32_t nodeId) const
{
    NS_ASSERT(nodeId < m_nodeInfos.size());
    return m_nodeInfos[nodeId].nvSwitch;
}

ApplicationContainer MultiNodeTopologyHelper::GetAllEndpoints() const { return m_allEndpoints; }

NodeContainer
MultiNodeTopologyHelper::Build()
{
    NS_LOG_FUNCTION(this);

    m_nodeInfos.resize(m_numNodes);

    for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
    {
        BuildIntraNode(nodeId);
    }

    BuildInterNodeLinks();

    return m_allNodes;
}

void
MultiNodeTopologyHelper::BuildIntraNode(uint32_t nodeId)
{
    NS_LOG_FUNCTION(this << nodeId);

    NodeInfo& info = m_nodeInfos[nodeId];
    info.nodeId = nodeId;
    info.baseRank = static_cast<uint16_t>(nodeId * m_gpusPerNode);
    info.gpuMacs.resize(m_gpusPerNode);
    info.gpuSwitchPorts.resize(m_gpusPerNode);

    // Create GPU nodes for this node
    info.gpuNodes.Create(m_gpusPerNode);
    for (uint32_t i = 0; i < m_gpusPerNode; ++i)
    {
        m_allGpuNodes.Add(info.gpuNodes.Get(i));
    }

    // Create FabricEndpoint applications
    FabricEndpointHelper endpointHelper;
    info.endpoints = endpointHelper.Install(info.gpuNodes);
    for (uint32_t i = 0; i < info.endpoints.GetN(); ++i)
    {
        m_allEndpoints.Add(info.endpoints.Get(i));
    }

    // Set rank, fabric type, node ID, and credits on each endpoint
    for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
    {
        uint16_t globalRank = GetGlobalRank(nodeId, localIdx);
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));
        endpointHelper.SetRank(info.endpoints.Get(localIdx), globalRank);
        ep->SetFabricType(m_intraNodeFabricType);
        ep->SetNodeId(nodeId);
        ep->SetVcCredits(0, m_vcCredits);
    }

    if (m_intraNodeTopology == "fullmesh")
    {
        BuildIntraNodeFullmesh(nodeId, info);
    }
    else
    {
        BuildIntraNodeSwitched(nodeId, info);
    }

    // Add all nodes from this node domain
    for (uint32_t i = 0; i < m_gpusPerNode; ++i)
    {
        m_allNodes.Add(info.gpuNodes.Get(i));
    }
}

void
MultiNodeTopologyHelper::BuildIntraNodeFullmesh(uint32_t nodeId, NodeInfo& info)
{
    NS_LOG_FUNCTION(this << nodeId << "fullmesh");

    // No switch node in fullmesh mode
    info.switchNode = nullptr;
    info.nvSwitch = nullptr;

    // Create p2p helper for intra-node links
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_intraNodeDataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_intraNodeDelay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // Connect every pair of GPUs with direct P2P links (m_numLanes per edge)
    for (uint32_t i = 0; i < m_gpusPerNode; ++i)
    {
        for (uint32_t j = i + 1; j < m_gpusPerNode; ++j)
        {
            Ptr<FabricEndpoint> epI = DynamicCast<FabricEndpoint>(info.endpoints.Get(i));
            Ptr<FabricEndpoint> epJ = DynamicCast<FabricEndpoint>(info.endpoints.Get(j));

            uint32_t logicalDevIdxI = epI->GetNNetDevices();
            uint32_t logicalDevIdxJ = epJ->GetNNetDevices();

            std::vector<uint32_t> physicalDevIndicesI;
            std::vector<uint32_t> physicalDevIndicesJ;

            for (uint32_t lane = 0; lane < m_numLanes; ++lane)
            {
                NetDeviceContainer devices = p2p.Install(info.gpuNodes.Get(i), info.gpuNodes.Get(j));
                epI->AddNetDevice(devices.Get(0));
                epJ->AddNetDevice(devices.Get(1));

                physicalDevIndicesI.push_back(epI->GetNNetDevices() - 1);
                physicalDevIndicesJ.push_back(epJ->GetNNetDevices() - 1);

                if (lane == 0)
                {
                    Mac48Address addrJ = Mac48Address::ConvertFrom(devices.Get(1)->GetAddress());
                    Mac48Address addrI = Mac48Address::ConvertFrom(devices.Get(0)->GetAddress());
                    info.gpuMacs[i].push_back(addrJ);
                    info.gpuMacs[j].push_back(addrI);
                }
            }

            // Register lane groups for sub-device spraying
            if (m_numLanes > 1)
            {
                epI->SetLaneGroup(logicalDevIdxI, physicalDevIndicesI);
                epJ->SetLaneGroup(logicalDevIdxJ, physicalDevIndicesJ);
            }

            uint16_t rankI = GetGlobalRank(nodeId, i);
            uint16_t rankJ = GetGlobalRank(nodeId, j);

            // Set neighbor MACs (using MACs stored from lane 0 above)
            epI->SetNeighborMac(rankJ, info.gpuMacs[i].back());
            epJ->SetNeighborMac(rankI, info.gpuMacs[j].back());

            epI->SetRoutingEntry(rankJ, logicalDevIdxI);
            epJ->SetRoutingEntry(rankI, logicalDevIdxJ);

            // Track logical device indices for proxy routing later
            info.localP2pDevIdxMap[{i, j}] = logicalDevIdxI;
            info.localP2pDevIdxMap[{j, i}] = logicalDevIdxJ;
        }
    }

    // Set neighbor MACs for intra-node destinations
    for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));
        ep->SetNumLanes(m_numLanes);
        if (m_numLanes > 1)
        {
            ep->SetSprayingEnabled(true);
            ep->SetSprayChunkSize(m_sprayChunkSize);
        }
    }
}

void
MultiNodeTopologyHelper::BuildIntraNodeSwitched(uint32_t nodeId, NodeInfo& info)
{
    NS_LOG_FUNCTION(this << nodeId << "switched");

    // Create NVSwitch node
    info.switchNode = CreateObject<Node>();
    m_allSwitchNodes.Add(info.switchNode);
    m_allNodes.Add(info.switchNode);

    NvSwitchHelper switchHelper;
    Ptr<NetDevice> swDev = switchHelper.Install(info.switchNode);
    info.nvSwitch = DynamicCast<NvSwitch>(swDev);
    NS_ASSERT(info.nvSwitch);

    info.nvSwitch->SetVoqDepth(m_switchVoqDepth);
    info.nvSwitch->SetArbitrationInterval(m_switchArbIntervalNs);
    if (m_switchCutThroughDelayNs > 0)
    {
        info.nvSwitch->SetCutThroughDelay(m_switchCutThroughDelayNs);
    }
    if (m_arbiter)
    {
        info.nvSwitch->SetArbiter(m_arbiter);
    }

    // Create p2p helper for intra-node links
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_intraNodeDataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_intraNodeDelay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // Connect each GPU to the NVSwitch
    for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));

        for (uint32_t link = 0; link < m_linksPerGpu; ++link)
        {
            NetDeviceContainer devPair = p2p.Install(info.gpuNodes.Get(localIdx), info.switchNode);
            Ptr<NetDevice> gpuDev = devPair.Get(0);
            Ptr<NetDevice> swPort = devPair.Get(1);

            ep->AddNetDevice(gpuDev);

            uint32_t portIdx = info.nvSwitch->AddPort(DynamicCast<PointToPointNetDevice>(swPort));
            info.gpuMacs[localIdx].push_back(Mac48Address::ConvertFrom(gpuDev->GetAddress()));
            info.gpuSwitchPorts[localIdx].push_back(portIdx);
        }
    }

    // Configure NVSwitch routing
    if (m_linksPerGpu > 1)
    {
        info.nvSwitch->SetSprayRouting(true);
        for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
        {
            info.nvSwitch->AddStaticRoute(info.gpuMacs[localIdx][0], info.gpuSwitchPorts[localIdx][0]);
            info.nvSwitch->AddSprayPorts(info.gpuMacs[localIdx][0], info.gpuSwitchPorts[localIdx]);
        }
    }
    else
    {
        for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
        {
            info.nvSwitch->AddStaticRoute(info.gpuMacs[localIdx][0], info.gpuSwitchPorts[localIdx][0]);
        }
    }

    // Configure intra-node endpoint routing
    for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));

        if (m_linksPerGpu > 1)
        {
            ep->SetSprayingEnabled(true);
            ep->SetSprayChunkSize(m_sprayChunkSize);
        }

        for (uint32_t destLocalIdx = 0; destLocalIdx < m_gpusPerNode; ++destLocalIdx)
        {
            if (destLocalIdx == localIdx) continue;

            uint16_t destRank = GetGlobalRank(nodeId, destLocalIdx);
            ep->SetNeighborMac(destRank, info.gpuMacs[destLocalIdx][0]);

            if (m_linksPerGpu > 1)
            {
                ep->SetRoutingEntry(destRank, 0);
                std::vector<uint32_t> routingDevices;
                for (uint32_t link = 0; link < m_linksPerGpu; ++link)
                {
                    routingDevices.push_back(link);
                }
                ep->SetRoutingDevices(destRank, routingDevices);
            }
            else
            {
                ep->SetRoutingEntry(destRank, 0);
            }
        }
    }
}

void
MultiNodeTopologyHelper::BuildInterNodeLinks()
{
    NS_LOG_FUNCTION(this);

    if (m_numNodes <= 1) return;

    PointToPointHelper p2pInter;
    p2pInter.SetDeviceAttribute("DataRate", StringValue(m_interNodeDataRate));
    p2pInter.SetChannelAttribute("Delay", StringValue(m_interNodeDelay));
    p2pInter.SetQueue("ns3::DropTailQueue<Packet>");

    if (m_interNodeTopology == "fullmesh")
    {
        // Connect same-local-rank GPUs across all node pairs
        for (uint32_t nodeA = 0; nodeA < m_numNodes; ++nodeA)
        {
            for (uint32_t nodeB = nodeA + 1; nodeB < m_numNodes; ++nodeB)
            {
                for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
                {
                    Ptr<Node> nodeAObj = m_nodeInfos[nodeA].gpuNodes.Get(localIdx);
                    Ptr<Node> nodeBObj = m_nodeInfos[nodeB].gpuNodes.Get(localIdx);

                    NetDeviceContainer devPair = p2pInter.Install(nodeAObj, nodeBObj);
                    Ptr<NetDevice> devA = devPair.Get(0);
                    Ptr<NetDevice> devB = devPair.Get(1);

                    Mac48Address macA = Mac48Address::ConvertFrom(devA->GetAddress());
                    Mac48Address macB = Mac48Address::ConvertFrom(devB->GetAddress());

                    Ptr<FabricEndpoint> epA = DynamicCast<FabricEndpoint>(
                        m_nodeInfos[nodeA].endpoints.Get(localIdx));
                    Ptr<FabricEndpoint> epB = DynamicCast<FabricEndpoint>(
                        m_nodeInfos[nodeB].endpoints.Get(localIdx));

                    uint32_t devIdxA = epA->AddNetDevice(devA);
                    uint32_t devIdxB = epB->AddNetDevice(devB);

                    // Store RDMA route info: (remoteNodeId, localIdx) -> {deviceIdx, peerMac}
                    m_nodeInfos[nodeA].rdmaRouteMap[{nodeB, localIdx}] = {devIdxA, macB};
                    m_nodeInfos[nodeB].rdmaRouteMap[{nodeA, localIdx}] = {devIdxB, macA};

                    m_nodeInfos[nodeA].interNodeDevices.push_back(devA);
                    m_nodeInfos[nodeB].interNodeDevices.push_back(devB);

                    NS_LOG_DEBUG("Inter-node link: node" << nodeA << "/gpu" << localIdx
                                << " <-> node" << nodeB << "/gpu" << localIdx
                                << " devIdx=" << devIdxA << "/" << devIdxB);
                }
            }
        }
    }
    else if (m_interNodeTopology == "ring")
    {
        // Ring: GPU 0 on each node connects to GPU 0 on neighbor nodes
        for (uint32_t nodeA = 0; nodeA < m_numNodes; ++nodeA)
        {
            uint32_t nodeB = (nodeA + 1) % m_numNodes;

            Ptr<Node> nodeAObj = m_nodeInfos[nodeA].gpuNodes.Get(0);
            Ptr<Node> nodeBObj = m_nodeInfos[nodeB].gpuNodes.Get(0);

            NetDeviceContainer devPair = p2pInter.Install(nodeAObj, nodeBObj);
            Ptr<NetDevice> devA = devPair.Get(0);
            Ptr<NetDevice> devB = devPair.Get(1);

            Mac48Address macA = Mac48Address::ConvertFrom(devA->GetAddress());
            Mac48Address macB = Mac48Address::ConvertFrom(devB->GetAddress());

            Ptr<FabricEndpoint> epA = DynamicCast<FabricEndpoint>(
                m_nodeInfos[nodeA].endpoints.Get(0));
            Ptr<FabricEndpoint> epB = DynamicCast<FabricEndpoint>(
                m_nodeInfos[nodeB].endpoints.Get(0));

            uint32_t devIdxA = epA->AddNetDevice(devA);
            uint32_t devIdxB = epB->AddNetDevice(devB);

            m_nodeInfos[nodeA].rdmaRouteMap[{nodeB, 0}] = {devIdxA, macB};
            m_nodeInfos[nodeB].rdmaRouteMap[{nodeA, 0}] = {devIdxB, macA};

            m_nodeInfos[nodeA].interNodeDevices.push_back(devA);
            m_nodeInfos[nodeB].interNodeDevices.push_back(devB);
        }
    }
    else if (m_interNodeTopology == "host")
    {
        NvSwitchHelper switchHelper;
        switchHelper.SetSwitchType(m_interNodeSwitchType);
        std::vector<Ptr<Node>> interNodeSwitchNodes;
        std::vector<Ptr<NvSwitch>> interNodeSwitches;
        interNodeSwitchNodes.resize(m_numNodes);
        interNodeSwitches.resize(m_numNodes);
        std::vector<std::vector<uint32_t>> gpuPorts(
            m_numNodes,
            std::vector<uint32_t>(m_gpusPerNode));
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> peerPorts;

        for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
        {
            Ptr<Node> switchNode = CreateObject<Node>();
            Ptr<NetDevice> switchDevice = switchHelper.Install(switchNode);
            Ptr<NvSwitch> hostSwitch = DynamicCast<NvSwitch>(switchDevice);
            NS_ASSERT(hostSwitch);
            hostSwitch->SetVoqDepth(m_switchVoqDepth);
            hostSwitch->SetArbitrationInterval(m_switchArbIntervalNs);
            if (m_switchCutThroughDelayNs > 0)
            {
                hostSwitch->SetCutThroughDelay(m_switchCutThroughDelayNs);
            }
            if (m_arbiter)
            {
                hostSwitch->SetArbiter(m_arbiter);
            }
            hostSwitch->SetLlrEnabled(m_interNodeSwitchLlrEnabled);
            interNodeSwitchNodes[nodeId] = switchNode;
            interNodeSwitches[nodeId] = hostSwitch;
            m_allSwitchNodes.Add(switchNode);
            m_allNodes.Add(switchNode);
        }

        for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
        {
            NodeInfo& info = m_nodeInfos[nodeId];
            for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
            {
                Ptr<Node> gpuNode = info.gpuNodes.Get(localIdx);
                Ptr<FabricEndpoint> ep =
                    DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));
                NetDeviceContainer devices =
                    p2pInter.Install(gpuNode, interNodeSwitchNodes[nodeId]);
                Ptr<NetDevice> gpuDevice = devices.Get(0);
                Ptr<NetDevice> switchPort = devices.Get(1);
                uint32_t deviceIndex = ep->AddNetDevice(gpuDevice);
                uint32_t portIndex =
                    interNodeSwitches[nodeId]->AddPort(
                        DynamicCast<PointToPointNetDevice>(switchPort));

                info.interNodeDevices.push_back(gpuDevice);
                info.interNodeDeviceIndices.push_back(deviceIndex);
                gpuPorts[nodeId][localIdx] = portIndex;
            }
        }

        for (uint32_t nodeA = 0; nodeA < m_numNodes; ++nodeA)
        {
            for (uint32_t nodeB = nodeA + 1; nodeB < m_numNodes; ++nodeB)
            {
                NetDeviceContainer devices =
                    p2pInter.Install(interNodeSwitchNodes[nodeA],
                                     interNodeSwitchNodes[nodeB]);
                peerPorts[{nodeA, nodeB}] = interNodeSwitches[nodeA]->AddPort(
                    DynamicCast<PointToPointNetDevice>(devices.Get(0)));
                peerPorts[{nodeB, nodeA}] = interNodeSwitches[nodeB]->AddPort(
                    DynamicCast<PointToPointNetDevice>(devices.Get(1)));
            }
        }

        for (uint32_t switchId = 0; switchId < m_numNodes; ++switchId)
        {
            Ptr<NvSwitch> hostSwitch = interNodeSwitches[switchId];
            for (uint32_t destNode = 0; destNode < m_numNodes; ++destNode)
            {
                for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
                {
                    Mac48Address destination = Mac48Address::ConvertFrom(
                        m_nodeInfos[destNode].interNodeDevices[localIdx]->GetAddress());
                    uint32_t port = destNode == switchId
                        ? gpuPorts[destNode][localIdx]
                        : peerPorts.at({switchId, destNode});
                    hostSwitch->AddStaticRoute(destination, port);
                }
            }
        }
    }
    else if (m_interNodeTopology == "tor")
    {
        // Single shared leaf switch (ib_write_lat's 2-NIC-into-1-leaf topology):
        // every node's inter-node NIC hangs off ONE switch, so a P2P transfer
        // crosses 1 switch hop (vs 'host's N switches + peer links = multi-hop).
        // For numNodes=2 this is the minimal same-switch RoCE fabric matching the
        // perftest; routing is identical to 'host' (one inter-node NIC per GPU,
        // the switch forwards by static route) so PopulateInterNodeRouting reuses
        // the 'host' block.
        NvSwitchHelper switchHelper;
        switchHelper.SetSwitchType(m_interNodeSwitchType);
        std::vector<Ptr<Node>> interNodeSwitchNodes;
        std::vector<Ptr<NvSwitch>> interNodeSwitches;
        interNodeSwitchNodes.resize(1);
        interNodeSwitches.resize(1);
        std::vector<std::vector<uint32_t>> gpuPorts(
            m_numNodes,
            std::vector<uint32_t>(m_gpusPerNode));

        Ptr<Node> switchNode = CreateObject<Node>();
        Ptr<NetDevice> switchDevice = switchHelper.Install(switchNode);
        Ptr<NvSwitch> leafSwitch = DynamicCast<NvSwitch>(switchDevice);
        NS_ASSERT(leafSwitch);
        leafSwitch->SetVoqDepth(m_switchVoqDepth);
        leafSwitch->SetArbitrationInterval(m_switchArbIntervalNs);
        if (m_switchCutThroughDelayNs > 0)
        {
            leafSwitch->SetCutThroughDelay(m_switchCutThroughDelayNs);
        }
        if (m_arbiter)
        {
            leafSwitch->SetArbiter(m_arbiter);
        }
        leafSwitch->SetLlrEnabled(m_interNodeSwitchLlrEnabled);
        interNodeSwitchNodes[0] = switchNode;
        interNodeSwitches[0] = leafSwitch;
        m_allSwitchNodes.Add(switchNode);
        m_allNodes.Add(switchNode);

        for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
        {
            NodeInfo& info = m_nodeInfos[nodeId];
            for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
            {
                Ptr<Node> gpuNode = info.gpuNodes.Get(localIdx);
                Ptr<FabricEndpoint> ep =
                    DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));
                NetDeviceContainer devices = p2pInter.Install(gpuNode, switchNode);
                Ptr<NetDevice> gpuDevice = devices.Get(0);
                Ptr<NetDevice> switchPort = devices.Get(1);
                uint32_t deviceIndex = ep->AddNetDevice(gpuDevice);
                uint32_t portIndex =
                    leafSwitch->AddPort(DynamicCast<PointToPointNetDevice>(switchPort));

                info.interNodeDevices.push_back(gpuDevice);
                info.interNodeDeviceIndices.push_back(deviceIndex);
                gpuPorts[nodeId][localIdx] = portIndex;
            }
        }

        // Static routes: the shared switch forwards each GPU MAC to its direct port.
        for (uint32_t destNode = 0; destNode < m_numNodes; ++destNode)
        {
            for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
            {
                Mac48Address destination = Mac48Address::ConvertFrom(
                    m_nodeInfos[destNode].interNodeDevices[localIdx]->GetAddress());
                leafSwitch->AddStaticRoute(destination, gpuPorts[destNode][localIdx]);
            }
        }
    }
    else if (m_interNodeTopology == "rail")
    {
        // Rail-optimized (DGX-SuperPOD compute-fabric pattern): gpusPerNode
        // rail switches shared by ALL nodes; the GPU with local index k on
        // every node hangs off rail switch k. Same-local-rank remote traffic
        // crosses ONE switch hop on its rail; cross-local-rank traffic first
        // moves intra-node to the same-local-rank proxy GPU (NCCL PXN) which
        // forwards on its rail -- the endpoint routing shape is identical to
        // 'fullmesh' (rdmaRouteMap + intra-node proxy), so
        // PopulateInterNodeRouting reuses the 'fullmesh' block.
        for (uint32_t rail = 0; rail < m_gpusPerNode; ++rail)
        {
            Ptr<Node> switchNode;
            Ptr<NvSwitch> railSwitch = CreateInterNodeSwitch(switchNode);

            std::vector<uint32_t> devIdx(m_numNodes);
            std::vector<uint32_t> port(m_numNodes);
            std::vector<Mac48Address> mac(m_numNodes);
            for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
            {
                NodeInfo& info = m_nodeInfos[nodeId];
                Ptr<Node> gpuNode = info.gpuNodes.Get(rail);
                Ptr<FabricEndpoint> ep =
                    DynamicCast<FabricEndpoint>(info.endpoints.Get(rail));
                NetDeviceContainer devices = p2pInter.Install(gpuNode, switchNode);
                Ptr<NetDevice> gpuDevice = devices.Get(0);
                Ptr<NetDevice> switchPort = devices.Get(1);
                devIdx[nodeId] = ep->AddNetDevice(gpuDevice);
                port[nodeId] = railSwitch->AddPort(
                    DynamicCast<PointToPointNetDevice>(switchPort));
                mac[nodeId] = Mac48Address::ConvertFrom(gpuDevice->GetAddress());
                info.interNodeDevices.push_back(gpuDevice);
                info.interNodeDeviceIndices.push_back(devIdx[nodeId]);
            }
            // All NIC MACs known: rail-switch static routes + endpoint
            // rdmaRouteMap (peer MAC = the remote GPU's rail NIC, which the
            // switch forwards by static route).
            for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
            {
                railSwitch->AddStaticRoute(mac[nodeId], port[nodeId]);
                for (uint32_t remote = 0; remote < m_numNodes; ++remote)
                {
                    if (remote == nodeId)
                    {
                        continue;
                    }
                    m_nodeInfos[nodeId].rdmaRouteMap[{remote, rail}] =
                        {devIdx[nodeId], mac[remote]};
                }
            }
        }
    }
    else if (m_interNodeTopology == "leafspine")
    {
        // Two-tier Clos: one leaf per node, numSpines spines (0 = auto
        // gpusPerNode = non-blocking at equal link rates), every leaf wired to
        // every spine by one link. Endpoint side is the same one-NIC-per-GPU
        // shape as 'host'/'tor' (PopulateInterNodeRouting reuses that block).
        // Each leaf spreads remote-destined MACs across spines by destination
        // local rank (destLocalIdx % numSpines -- deterministic, rail-aligned
        // ECMP; static per-MAC routes cannot hash per flow).
        uint32_t numSpines = m_interNodeSpines ? m_interNodeSpines : m_gpusPerNode;
        std::vector<Ptr<Node>> leafNodes(m_numNodes);
        std::vector<Ptr<NvSwitch>> leaves(m_numNodes);
        std::vector<Ptr<Node>> spineNodes(numSpines);
        std::vector<Ptr<NvSwitch>> spines(numSpines);
        std::vector<std::vector<uint32_t>> gpuPorts(
            m_numNodes, std::vector<uint32_t>(m_gpusPerNode));
        std::vector<std::vector<uint32_t>> leafUpPorts(
            m_numNodes, std::vector<uint32_t>(numSpines));
        std::vector<std::vector<uint32_t>> spineDownPorts(
            numSpines, std::vector<uint32_t>(m_numNodes));

        for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
        {
            leaves[nodeId] = CreateInterNodeSwitch(leafNodes[nodeId]);
        }
        for (uint32_t s = 0; s < numSpines; ++s)
        {
            spines[s] = CreateInterNodeSwitch(spineNodes[s]);
        }

        for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
        {
            NodeInfo& info = m_nodeInfos[nodeId];
            for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
            {
                Ptr<Node> gpuNode = info.gpuNodes.Get(localIdx);
                Ptr<FabricEndpoint> ep =
                    DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));
                NetDeviceContainer devices = p2pInter.Install(gpuNode, leafNodes[nodeId]);
                Ptr<NetDevice> gpuDevice = devices.Get(0);
                Ptr<NetDevice> switchPort = devices.Get(1);
                uint32_t deviceIndex = ep->AddNetDevice(gpuDevice);
                gpuPorts[nodeId][localIdx] = leaves[nodeId]->AddPort(
                    DynamicCast<PointToPointNetDevice>(switchPort));
                info.interNodeDevices.push_back(gpuDevice);
                info.interNodeDeviceIndices.push_back(deviceIndex);
            }
            for (uint32_t s = 0; s < numSpines; ++s)
            {
                NetDeviceContainer devices =
                    p2pInter.Install(leafNodes[nodeId], spineNodes[s]);
                leafUpPorts[nodeId][s] = leaves[nodeId]->AddPort(
                    DynamicCast<PointToPointNetDevice>(devices.Get(0)));
                spineDownPorts[s][nodeId] = spines[s]->AddPort(
                    DynamicCast<PointToPointNetDevice>(devices.Get(1)));
            }
        }

        for (uint32_t destNode = 0; destNode < m_numNodes; ++destNode)
        {
            for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
            {
                Mac48Address destination = Mac48Address::ConvertFrom(
                    m_nodeInfos[destNode].interNodeDevices[localIdx]->GetAddress());
                uint32_t spine = localIdx % numSpines;
                for (uint32_t leafId = 0; leafId < m_numNodes; ++leafId)
                {
                    leaves[leafId]->AddStaticRoute(
                        destination,
                        leafId == destNode ? gpuPorts[destNode][localIdx]
                                           : leafUpPorts[leafId][spine]);
                }
                spines[spine]->AddStaticRoute(destination, spineDownPorts[spine][destNode]);
            }
        }
    }
    else if (m_interNodeTopology == "torus2d" || m_interNodeTopology == "dragonfly")
    {
        // Gateway direct topologies ('ring' generalized): GPU 0 on each node
        // is the inter-node gateway; multi-hop paths are forwarded hop-by-hop
        // by the intermediate gateways' own routing tables (FabricEndpoint
        // forwards packets whose destRank is not its own).
        //   torus2d:   W x H node grid, +/-x and +/-y neighbor links,
        //              dimension-order shortest-wrap routing.
        //   dragonfly: groups of g nodes, intra-group gateway fullmesh + one
        //              global link per group pair (palmtree assignment),
        //              minimal l-g-l routing (max 3 hops).
        // Both build only DIRECT gateway-gateway links here; next hops are
        // computed by GatewayNextHop in PopulateInterNodeRouting.
        std::vector<std::pair<uint32_t, uint32_t>> links;
        if (m_interNodeTopology == "torus2d")
        {
            uint32_t width = TorusWidth();
            uint32_t height = m_numNodes / width;
            for (uint32_t y = 0; y < height; ++y)
            {
                for (uint32_t x = 0; x < width; ++x)
                {
                    // +x / +y wrap links; for a dimension of size 2 the wrap
                    // link duplicates the forward link, so emit it only once
                    if (width > 1 && !(width == 2 && x == 1))
                    {
                        links.push_back({y * width + x, y * width + (x + 1) % width});
                    }
                    if (height > 1 && !(height == 2 && y == 1))
                    {
                        links.push_back({y * width + x, ((y + 1) % height) * width + x});
                    }
                }
            }
        }
        else
        {
            uint32_t groupSize = DragonflyGroupSize();
            uint32_t numGroups = m_numNodes / groupSize;
            // Intra-group gateway fullmesh
            for (uint32_t g = 0; g < numGroups; ++g)
            {
                for (uint32_t a = 0; a < groupSize; ++a)
                {
                    for (uint32_t b = a + 1; b < groupSize; ++b)
                    {
                        links.push_back({g * groupSize + a, g * groupSize + b});
                    }
                }
            }
            // One global link per group pair (palmtree assignment, mirrored
            // in GatewayNextHop)
            for (uint32_t ga = 0; ga < numGroups; ++ga)
            {
                for (uint32_t gb = ga + 1; gb < numGroups; ++gb)
                {
                    // Holder in ga for gb: (gb-1) % g; holder in gb for ga:
                    // ga % g (ga < gb) -- matches GatewayNextHop
                    uint32_t nodeA = ga * groupSize + (gb - 1) % groupSize;
                    uint32_t nodeB = gb * groupSize + ga % groupSize;
                    links.push_back({nodeA, nodeB});
                }
            }
        }

        for (const auto& link : links)
        {
            uint32_t nodeA = link.first;
            uint32_t nodeB = link.second;
            Ptr<Node> nodeAObj = m_nodeInfos[nodeA].gpuNodes.Get(0);
            Ptr<Node> nodeBObj = m_nodeInfos[nodeB].gpuNodes.Get(0);

            NetDeviceContainer devPair = p2pInter.Install(nodeAObj, nodeBObj);
            Ptr<NetDevice> devA = devPair.Get(0);
            Ptr<NetDevice> devB = devPair.Get(1);

            Ptr<FabricEndpoint> epA = DynamicCast<FabricEndpoint>(
                m_nodeInfos[nodeA].endpoints.Get(0));
            Ptr<FabricEndpoint> epB = DynamicCast<FabricEndpoint>(
                m_nodeInfos[nodeB].endpoints.Get(0));

            uint32_t devIdxA = epA->AddNetDevice(devA);
            uint32_t devIdxB = epB->AddNetDevice(devB);

            m_nodeInfos[nodeA].rdmaRouteMap[{nodeB, 0}] =
                {devIdxA, Mac48Address::ConvertFrom(devB->GetAddress())};
            m_nodeInfos[nodeB].rdmaRouteMap[{nodeA, 0}] =
                {devIdxB, Mac48Address::ConvertFrom(devA->GetAddress())};

            m_nodeInfos[nodeA].interNodeDevices.push_back(devA);
            m_nodeInfos[nodeB].interNodeDevices.push_back(devB);
        }
    }
    else
    {
        NS_ABORT_MSG("Unsupported inter-node topology: " << m_interNodeTopology);
    }
}

void
MultiNodeTopologyHelper::PopulateInterNodeRouting()
{
    NS_LOG_FUNCTION(this);

    if (m_numNodes <= 1) return;

    // One-inter-node-NIC-per-GPU switched topologies: every remote rank goes
    // out the GPU's own NIC; the switch tier forwards by static route.
    if (m_interNodeTopology == "host" || m_interNodeTopology == "tor" ||
        m_interNodeTopology == "leafspine")
    {
        for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
        {
            NodeInfo& info = m_nodeInfos[nodeId];
            for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
            {
                Ptr<FabricEndpoint> ep =
                    DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));
                ep->SetInterNodeFabricType(m_interNodeFabricType);
                ep->SetLocalRankRange(info.baseRank, m_gpusPerNode);

                for (uint32_t remoteNodeId = 0; remoteNodeId < m_numNodes; ++remoteNodeId)
                {
                    if (remoteNodeId == nodeId)
                    {
                        continue;
                    }
                    for (uint32_t remoteLocalIdx = 0;
                         remoteLocalIdx < m_gpusPerNode;
                         ++remoteLocalIdx)
                    {
                        uint16_t remoteRank = GetGlobalRank(remoteNodeId, remoteLocalIdx);
                        Ptr<NetDevice> remoteDevice =
                            m_nodeInfos[remoteNodeId].interNodeDevices[remoteLocalIdx];
                        ep->SetRoutingEntry(remoteRank, info.interNodeDeviceIndices[localIdx]);
                        ep->SetNeighborMac(
                            remoteRank,
                            Mac48Address::ConvertFrom(remoteDevice->GetAddress()));
                    }
                }
            }
        }
        return;
    }

    for (uint32_t nodeId = 0; nodeId < m_numNodes; ++nodeId)
    {
        NodeInfo& info = m_nodeInfos[nodeId];

        for (uint32_t localIdx = 0; localIdx < m_gpusPerNode; ++localIdx)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(info.endpoints.Get(localIdx));
            ep->SetInterNodeFabricType(m_interNodeFabricType);
            ep->SetLocalRankRange(info.baseRank, m_gpusPerNode);

            for (uint32_t remoteNodeId = 0; remoteNodeId < m_numNodes; ++remoteNodeId)
            {
                if (remoteNodeId == nodeId) continue;

                for (uint32_t remoteLocalIdx = 0; remoteLocalIdx < m_gpusPerNode; ++remoteLocalIdx)
                {
                    uint16_t remoteRank = GetGlobalRank(remoteNodeId, remoteLocalIdx);

                    if (m_interNodeTopology == "fullmesh" || m_interNodeTopology == "rail")
                    {
                        // Same-local-rank has a direct path out this GPU's own
                        // inter-node NIC (fullmesh: private RDMA link; rail:
                        // one switch hop on this local rank's rail)
                        if (remoteLocalIdx == localIdx)
                        {
                            auto it = info.rdmaRouteMap.find({remoteNodeId, localIdx});
                            NS_ASSERT(it != info.rdmaRouteMap.end());
                            uint32_t rdmaDevIdx = it->second.first;
                            Mac48Address peerMac = it->second.second;
                            ep->SetRoutingEntry(remoteRank, rdmaDevIdx);
                            ep->SetNeighborMac(remoteRank, peerMac);
                        }
                        else
                        {
                            // Different local-rank on remote node: route via local P2P link
                            // to same-local-rank peer, which forwards via RDMA NIC
                            uint16_t proxyRank = GetGlobalRank(nodeId, remoteLocalIdx);
                            ep->SetNeighborMac(remoteRank, ep->GetNeighborMac(proxyRank));

                            if (m_intraNodeTopology == "fullmesh")
                            {
                                // Use the logical P2P device index that connects to proxy peer
                                auto it = info.localP2pDevIdxMap.find({localIdx, remoteLocalIdx});
                                NS_ASSERT(it != info.localP2pDevIdxMap.end());
                                ep->SetRoutingEntry(remoteRank, it->second);
                            }
                            else
                            {
                                // Switched mode: route via NVSwitch (device 0)
                                ep->SetRoutingEntry(remoteRank, 0);
                            }
                        }
                    }
                    else if (m_interNodeTopology == "ring")
                    {
                        // Ring: all remote traffic goes through GPU 0 (gateway)
                        if (localIdx == 0)
                        {
                            // GPU 0 has RDMA links - use ring-distance to pick closer NIC
                            uint32_t forwardDist = (remoteNodeId - nodeId + m_numNodes) % m_numNodes;
                            uint32_t backwardDist = (nodeId - remoteNodeId + m_numNodes) % m_numNodes;
                            uint32_t rdmaDevIdx;
                            if (forwardDist <= backwardDist)
                            {
                                auto it = info.rdmaRouteMap.find({(nodeId + 1) % m_numNodes, 0});
                                NS_ASSERT(it != info.rdmaRouteMap.end());
                                rdmaDevIdx = it->second.first;
                            }
                            else
                            {
                                auto it = info.rdmaRouteMap.find({(nodeId - 1 + m_numNodes) % m_numNodes, 0});
                                NS_ASSERT(it != info.rdmaRouteMap.end());
                                rdmaDevIdx = it->second.first;
                            }
                            Mac48Address peerMac = Mac48Address::ConvertFrom(
                                m_nodeInfos[remoteNodeId].interNodeDevices[0]->GetAddress());
                            ep->SetRoutingEntry(remoteRank, rdmaDevIdx);
                            ep->SetNeighborMac(remoteRank, peerMac);
                        }
                        else
                        {
                            // Non-gateway GPUs route via NVSwitch to GPU 0
                            ep->SetRoutingEntry(remoteRank, 0);
                            ep->SetNeighborMac(remoteRank, info.gpuMacs[0][0]);
                        }
                    }
                    else if (m_interNodeTopology == "torus2d" ||
                             m_interNodeTopology == "dragonfly")
                    {
                        // Gateway topologies with hop-by-hop forwarding: GPU 0
                        // routes toward GatewayNextHop's direct link; the
                        // intermediate gateways' own routing tables carry the
                        // packet the rest of the way (FabricEndpoint forwards
                        // packets whose destRank is not its own).
                        NS_ABORT_MSG_IF(m_intraNodeTopology != "switched",
                                        m_interNodeTopology
                                        << " gateway routing requires "
                                           "intraNodeTopology=switched");
                        if (localIdx == 0)
                        {
                            uint32_t nextHop = GatewayNextHop(nodeId, remoteNodeId);
                            auto it = info.rdmaRouteMap.find({nextHop, 0});
                            NS_ASSERT(it != info.rdmaRouteMap.end());
                            Mac48Address peerMac = Mac48Address::ConvertFrom(
                                m_nodeInfos[remoteNodeId].interNodeDevices[0]->GetAddress());
                            ep->SetRoutingEntry(remoteRank, it->second.first);
                            ep->SetNeighborMac(remoteRank, peerMac);
                        }
                        else
                        {
                            // Non-gateway GPUs route via NVSwitch to GPU 0
                            ep->SetRoutingEntry(remoteRank, 0);
                            ep->SetNeighborMac(remoteRank, info.gpuMacs[0][0]);
                        }
                    }
                }
            }
        }
    }
}

} // namespace ns3
