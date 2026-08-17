/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Helper classes implementation for GPU Cluster network configuration
 */

#include "gpu-cluster-helper.h"

#include "ns3/fabric-endpoint.h"
#include "ns3/fabric-switch.h"
#include "ns3/nvswitch.h"
#include "ns3/contention-model.h"
#include "ns3/log.h"
#include "ns3/string.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/mac48-address.h"
#include "ns3/data-rate.h"
#include <algorithm>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>

namespace ns3
{

NS_LOG_COMPONENT_DEFINE("GpuClusterHelper");

// FabricEndpointHelper implementation

FabricEndpointHelper::FabricEndpointHelper()
{
    m_factory.SetTypeId(FabricEndpoint::GetTypeId());
}

void
FabricEndpointHelper::SetAttribute(std::string name, const AttributeValue& value)
{
    m_factory.Set(name, value);
}

ApplicationContainer
FabricEndpointHelper::Install(Ptr<Node> node) const
{
    return ApplicationContainer(InstallPriv(node));
}

ApplicationContainer
FabricEndpointHelper::Install(NodeContainer nodes) const
{
    ApplicationContainer apps;
    for (auto i = nodes.Begin(); i != nodes.End(); ++i)
    {
        apps.Add(InstallPriv(*i));
    }
    return apps;
}

Ptr<Application>
FabricEndpointHelper::InstallPriv(Ptr<Node> node) const
{
    NS_LOG_FUNCTION(node);

    Ptr<FabricEndpoint> app = m_factory.Create<FabricEndpoint>();
    node->AddApplication(app);

    return app;
}

void
FabricEndpointHelper::AddNetDevice(Ptr<Application> app, Ptr<NetDevice> device) const
{
    NS_LOG_FUNCTION(app << device);

    Ptr<FabricEndpoint> endpoint = DynamicCast<FabricEndpoint>(app);
    if (endpoint)
    {
        endpoint->AddNetDevice(device);
    }
}

void
FabricEndpointHelper::SetRank(Ptr<Application> app, uint16_t rank) const
{
    NS_LOG_FUNCTION(app << rank);

    Ptr<FabricEndpoint> endpoint = DynamicCast<FabricEndpoint>(app);
    if (endpoint)
    {
        endpoint->SetRank(rank);
    }
}

// NvSwitchHelper implementation

NvSwitchHelper::NvSwitchHelper()
{
    m_factory.SetTypeId(NvSwitch::GetTypeId());
}

void
NvSwitchHelper::SetAttribute(std::string name, const AttributeValue& value)
{
    m_factory.Set(name, value);
}

void
NvSwitchHelper::SetSwitchType(const std::string& typeId)
{
    NS_LOG_FUNCTION(this << typeId);
    // Validate the TypeId exists and is a FabricSwitch subclass so a typo
    // fails loudly at configuration time rather than as a null install.
    TypeId tid = TypeId::LookupByName(typeId);
    if (!tid.IsChildOf(FabricSwitch::GetTypeId()) && tid != FabricSwitch::GetTypeId())
    {
        NS_FATAL_ERROR("NvSwitchHelper::SetSwitchType: " << typeId
                      << " is not a ns3::FabricSwitch subclass");
    }
    m_factory.SetTypeId(typeId);
}

Ptr<NetDevice>
NvSwitchHelper::Install(Ptr<Node> node) const
{
    NS_LOG_FUNCTION(node);

    Ptr<FabricSwitch> sw = m_factory.Create<FabricSwitch>();
    sw->SetNode(node);
    node->AddDevice(sw);

    return sw;
}

uint32_t
NvSwitchHelper::AddPort(Ptr<NetDevice> switchDevice, Ptr<NetDevice> portDevice) const
{
    NS_LOG_FUNCTION(switchDevice << portDevice);

    Ptr<FabricSwitch> sw = DynamicCast<FabricSwitch>(switchDevice);
    if (sw)
    {
        return sw->AddPort(portDevice);
    }
    return 0;
}

void
NvSwitchHelper::ConnectSwitches(Ptr<NetDevice> switch1, Ptr<NetDevice> switch2,
                                 Ptr<Channel> channel) const
{
    NS_LOG_FUNCTION(switch1 << switch2 << channel);
    NS_LOG_WARN("ConnectSwitches is not implemented; use BuildFatTree or BuildFullyConnected instead");
}

// GpuClusterTopologyHelper implementation

GpuClusterTopologyHelper::GpuClusterTopologyHelper(uint32_t numGpus, uint32_t numSwitches)
    : m_numGpus(numGpus),
      m_numSwitches(numSwitches),
      m_dataRate("100Gbps"),
      m_delay("500ns"),
      m_fabricId(0),
      m_fabricType(static_cast<uint8_t>(FabricType::NVLINK)),
      m_linksPerGpu(1),
      m_sprayChunkSize(131072),
      m_numLanes(1)
{
    NS_LOG_FUNCTION(numGpus << numSwitches);
}

void
GpuClusterTopologyHelper::SetLinkDataRate(std::string rate)
{
    m_dataRate = rate;
}

void
GpuClusterTopologyHelper::SetLinkDelay(std::string delay)
{
    m_delay = delay;
}

namespace
{

Ptr<NvSwitch>
FindNvSwitchOnNode(Ptr<Node> node)
{
    if (!node)
    {
        return nullptr;
    }
    for (uint32_t device = 0; device < node->GetNDevices(); ++device)
    {
        Ptr<NvSwitch> sw = DynamicCast<NvSwitch>(node->GetDevice(device));
        if (sw)
        {
            return sw;
        }
    }
    return nullptr;
}

Ptr<PointToPointNetDevice>
GetPeerDevice(Ptr<PointToPointNetDevice> device)
{
    if (!device)
    {
        return nullptr;
    }
    Ptr<PointToPointChannel> channel =
        DynamicCast<PointToPointChannel>(device->GetChannel());
    if (!channel)
    {
        return nullptr;
    }
    Ptr<PointToPointNetDevice> first = channel->GetPointToPointDevice(0);
    Ptr<PointToPointNetDevice> second = channel->GetPointToPointDevice(1);
    return first == device ? second : first;
}

int32_t
FindSwitchPort(Ptr<NvSwitch> sw, Ptr<NetDevice> device)
{
    if (!sw || !device)
    {
        return -1;
    }
    for (uint32_t port = 0; port < sw->GetNPorts(); ++port)
    {
        if (sw->GetPort(port) == device)
        {
            return static_cast<int32_t>(port);
        }
    }
    return -1;
}

bool
SwitchPortIsUp(Ptr<NvSwitch> sw, uint32_t port)
{
    if (!sw || port >= sw->GetNPorts())
    {
        return false;
    }
    Ptr<NetDevice> device = sw->GetPort(port);
    if (!device || !device->IsLinkUp())
    {
        return false;
    }
    Ptr<LinkDegradationModel> model = sw->GetPortDegradationModel(port);
    return !model || model->IsLinkUp();
}

} // namespace

std::vector<GpuClusterTopologyHelper::InterSwitchLink>
GpuClusterTopologyHelper::DiscoverOpticalInterSwitchLinks() const
{
    std::vector<InterSwitchLink> links;
    std::set<const Channel*> visitedChannels;

    for (auto nodeIt = m_switchNodes.Begin(); nodeIt != m_switchNodes.End(); ++nodeIt)
    {
        Ptr<Node> leftNode = *nodeIt;
        Ptr<NvSwitch> leftSwitch = FindNvSwitchOnNode(leftNode);
        if (!leftSwitch)
        {
            continue;
        }
        for (uint32_t leftPort = 0; leftPort < leftSwitch->GetNPorts(); ++leftPort)
        {
            Ptr<PointToPointNetDevice> leftDevice =
                DynamicCast<PointToPointNetDevice>(leftSwitch->GetPort(leftPort));
            if (!leftDevice || !leftDevice->GetChannel())
            {
                continue;
            }
            const Channel* channelKey = PeekPointer(leftDevice->GetChannel());
            if (visitedChannels.count(channelKey) != 0)
            {
                continue;
            }

            Ptr<PointToPointNetDevice> rightDevice = GetPeerDevice(leftDevice);
            Ptr<Node> rightNode = rightDevice ? rightDevice->GetNode() : nullptr;
            Ptr<NvSwitch> rightSwitch = FindNvSwitchOnNode(rightNode);
            if (!rightSwitch)
            {
                continue;
            }
            const int32_t rightPort = FindSwitchPort(rightSwitch, rightDevice);
            if (rightPort < 0)
            {
                continue;
            }

            Ptr<LinkDegradationModel> leftModel =
                leftSwitch->GetPortDegradationModel(leftPort);
            Ptr<LinkDegradationModel> rightModel =
                rightSwitch->GetPortDegradationModel(static_cast<uint32_t>(rightPort));
            const std::string leftMedium =
                leftModel ? leftModel->GetLinkMetadata().medium : "";
            const std::string rightMedium =
                rightModel ? rightModel->GetLinkMetadata().medium : "";
            if (leftMedium != "optical" && rightMedium != "optical")
            {
                continue;
            }

            visitedChannels.insert(channelKey);
            InterSwitchLink link;
            link.leftSwitch = leftSwitch;
            link.rightSwitch = rightSwitch;
            link.leftPort = leftPort;
            link.rightPort = static_cast<uint32_t>(rightPort);
            link.leftNodeId = leftNode->GetId();
            link.rightNodeId = rightNode->GetId();
            link.medium = "optical";
            link.ber = leftModel ? leftModel->GetBer()
                                 : (rightModel ? rightModel->GetBer() : 0.0);
            links.push_back(link);
        }
    }

    std::sort(links.begin(), links.end(), [](const InterSwitchLink& a,
                                             const InterSwitchLink& b) {
        const auto aNodes = std::minmax(a.leftNodeId, a.rightNodeId);
        const auto bNodes = std::minmax(b.leftNodeId, b.rightNodeId);
        if (aNodes.first != bNodes.first)
        {
            return aNodes.first < bNodes.first;
        }
        if (aNodes.second != bNodes.second)
        {
            return aNodes.second < bNodes.second;
        }
        if (a.leftPort != b.leftPort)
        {
            return a.leftPort < b.leftPort;
        }
        return a.rightPort < b.rightPort;
    });
    return links;
}

uint32_t
GpuClusterTopologyHelper::GetOpticalInterSwitchLinkCount() const
{
    return static_cast<uint32_t>(DiscoverOpticalInterSwitchLinks().size());
}

uint32_t
GpuClusterTopologyHelper::GetOperationalOpticalInterSwitchLinkCount() const
{
    uint32_t count = 0;
    for (const InterSwitchLink& link : DiscoverOpticalInterSwitchLinks())
    {
        if (SwitchPortIsUp(link.leftSwitch, link.leftPort)
            && SwitchPortIsUp(link.rightSwitch, link.rightPort))
        {
            ++count;
        }
    }
    return count;
}

double
GpuClusterTopologyHelper::GetOperationalOpticalInterSwitchBerMin() const
{
    double minimum = std::numeric_limits<double>::infinity();
    for (const InterSwitchLink& link : DiscoverOpticalInterSwitchLinks())
    {
        if (SwitchPortIsUp(link.leftSwitch, link.leftPort)
            && SwitchPortIsUp(link.rightSwitch, link.rightPort))
        {
            minimum = std::min(minimum, link.ber);
        }
    }
    return std::isfinite(minimum) ? minimum : 0.0;
}

double
GpuClusterTopologyHelper::GetOperationalOpticalInterSwitchBerMax() const
{
    double maximum = 0.0;
    for (const InterSwitchLink& link : DiscoverOpticalInterSwitchLinks())
    {
        if (SwitchPortIsUp(link.leftSwitch, link.leftPort)
            && SwitchPortIsUp(link.rightSwitch, link.rightPort))
        {
            maximum = std::max(maximum, link.ber);
        }
    }
    return maximum;
}

bool
GpuClusterTopologyHelper::FailOpticalInterSwitchLink(uint32_t index)
{
    const std::vector<InterSwitchLink> links = DiscoverOpticalInterSwitchLinks();
    if (index >= links.size())
    {
        NS_LOG_ERROR("Optical inter-switch link index " << index
                     << " is outside [0," << links.size() << ")");
        return false;
    }

    const InterSwitchLink& failed = links[index];
    Ptr<LinkDegradationModel> leftModel =
        failed.leftSwitch->GetPortDegradationModel(failed.leftPort);
    Ptr<LinkDegradationModel> rightModel =
        failed.rightSwitch->GetPortDegradationModel(failed.rightPort);
    if (!leftModel || !rightModel)
    {
        NS_LOG_ERROR("Selected optical link has no degradation model on both endpoints");
        return false;
    }

    leftModel->SetLinkState(LinkState::DOWN);
    rightModel->SetLinkState(LinkState::DOWN);
    m_failedOpticalLinkIndex = static_cast<int32_t>(index);
    m_failedOpticalLinkBer = failed.ber;
    std::ostringstream description;
    description << failed.leftNodeId << ":" << failed.leftPort
                << "<->" << failed.rightNodeId << ":" << failed.rightPort;
    m_failedOpticalLinkDescription = description.str();
    return RecomputeFailureAwareRoutes();
}

int32_t
GpuClusterTopologyHelper::GetFailedOpticalLinkIndex() const
{
    return m_failedOpticalLinkIndex;
}

std::string
GpuClusterTopologyHelper::GetFailedOpticalLinkDescription() const
{
    return m_failedOpticalLinkDescription;
}

double
GpuClusterTopologyHelper::GetFailedOpticalLinkBer() const
{
    return m_failedOpticalLinkBer;
}

uint64_t
GpuClusterTopologyHelper::GetUnreachableGpuPairs() const
{
    return m_unreachableGpuPairs;
}

bool
GpuClusterTopologyHelper::RecomputeFailureAwareRoutes()
{
    struct RouteEdge
    {
        uint32_t nextSwitch;
        uint32_t outputPort;
    };
    struct Attachment
    {
        uint32_t switchIndex;
        uint32_t switchPort;
    };

    std::vector<Ptr<NvSwitch>> switches;
    std::unordered_map<uint32_t, uint32_t> nodeToSwitch;
    for (auto nodeIt = m_switchNodes.Begin(); nodeIt != m_switchNodes.End(); ++nodeIt)
    {
        Ptr<NvSwitch> sw = FindNvSwitchOnNode(*nodeIt);
        if (!sw)
        {
            continue;
        }
        nodeToSwitch[(*nodeIt)->GetId()] = switches.size();
        switches.push_back(sw);
    }
    if (switches.empty())
    {
        NS_LOG_ERROR("Failure-aware routing requires a switched topology");
        return false;
    }

    std::vector<std::vector<RouteEdge>> adjacency(switches.size());
    for (uint32_t swIndex = 0; swIndex < switches.size(); ++swIndex)
    {
        Ptr<NvSwitch> sw = switches[swIndex];
        for (uint32_t port = 0; port < sw->GetNPorts(); ++port)
        {
            Ptr<PointToPointNetDevice> localDevice =
                DynamicCast<PointToPointNetDevice>(sw->GetPort(port));
            Ptr<PointToPointNetDevice> peerDevice = GetPeerDevice(localDevice);
            Ptr<Node> peerNode = peerDevice ? peerDevice->GetNode() : nullptr;
            if (!peerNode)
            {
                continue;
            }
            auto peer = nodeToSwitch.find(peerNode->GetId());
            if (peer == nodeToSwitch.end())
            {
                continue;
            }
            adjacency[swIndex].push_back({peer->second, port});
        }
        sw->ClearRoutingTables();
        sw->SetSourceBasedRouting(false);
        sw->SetSprayRouting(true);
        sw->SetFailureAwareRouting(true);
    }

    struct Destination
    {
        uint16_t rank;
        Mac48Address mac;
        std::vector<Attachment> attachments;
    };
    std::vector<Destination> destinations;
    destinations.reserve(m_endpoints.GetN());
    for (uint32_t appIndex = 0; appIndex < m_endpoints.GetN(); ++appIndex)
    {
        Ptr<FabricEndpoint> endpoint =
            DynamicCast<FabricEndpoint>(m_endpoints.Get(appIndex));
        if (!endpoint)
        {
            continue;
        }
        Destination destination;
        destination.rank = endpoint->GetRank();
        destination.mac = endpoint->GetAddress();
        for (uint32_t deviceIndex = 0;
             deviceIndex < endpoint->GetNNetDevices();
             ++deviceIndex)
        {
            Ptr<PointToPointNetDevice> endpointDevice =
                DynamicCast<PointToPointNetDevice>(endpoint->GetNetDevice(deviceIndex));
            Ptr<PointToPointNetDevice> switchDevice = GetPeerDevice(endpointDevice);
            Ptr<Node> switchNode = switchDevice ? switchDevice->GetNode() : nullptr;
            if (!switchNode)
            {
                continue;
            }
            auto swIndex = nodeToSwitch.find(switchNode->GetId());
            if (swIndex == nodeToSwitch.end())
            {
                continue;
            }
            const int32_t switchPort = FindSwitchPort(switches[swIndex->second], switchDevice);
            if (switchPort >= 0)
            {
                destination.attachments.push_back(
                    {swIndex->second, static_cast<uint32_t>(switchPort)});
            }
        }
        destinations.push_back(destination);
    }

    const uint32_t unreachable = std::numeric_limits<uint32_t>::max();
    m_unreachableGpuPairs = 0;
    for (const Destination& destination : destinations)
    {
        std::vector<uint32_t> distance(switches.size(), unreachable);
        std::queue<uint32_t> work;
        for (const Attachment& attachment : destination.attachments)
        {
            if (SwitchPortIsUp(switches[attachment.switchIndex], attachment.switchPort)
                && distance[attachment.switchIndex] == unreachable)
            {
                distance[attachment.switchIndex] = 0;
                work.push(attachment.switchIndex);
            }
        }
        while (!work.empty())
        {
            const uint32_t current = work.front();
            work.pop();
            for (const RouteEdge& edge : adjacency[current])
            {
                if (!SwitchPortIsUp(switches[current], edge.outputPort)
                    || distance[edge.nextSwitch] != unreachable)
                {
                    continue;
                }
                distance[edge.nextSwitch] = distance[current] + 1;
                work.push(edge.nextSwitch);
            }
        }

        for (uint32_t swIndex = 0; swIndex < switches.size(); ++swIndex)
        {
            if (distance[swIndex] == unreachable)
            {
                continue;
            }
            std::vector<uint32_t> outputPorts;
            if (distance[swIndex] == 0)
            {
                for (const Attachment& attachment : destination.attachments)
                {
                    if (attachment.switchIndex == swIndex
                        && SwitchPortIsUp(switches[swIndex], attachment.switchPort))
                    {
                        outputPorts.push_back(attachment.switchPort);
                    }
                }
            }
            else
            {
                for (const RouteEdge& edge : adjacency[swIndex])
                {
                    if (SwitchPortIsUp(switches[swIndex], edge.outputPort)
                        && distance[edge.nextSwitch] != unreachable
                        && distance[edge.nextSwitch] + 1 == distance[swIndex])
                    {
                        outputPorts.push_back(edge.outputPort);
                    }
                }
            }
            if (!outputPorts.empty())
            {
                std::sort(outputPorts.begin(), outputPorts.end());
                outputPorts.erase(std::unique(outputPorts.begin(), outputPorts.end()),
                                  outputPorts.end());
                switches[swIndex]->AddStaticRoute(destination.mac, outputPorts.front());
                switches[swIndex]->AddSprayPorts(destination.mac, outputPorts);
            }
        }

        for (const Destination& source : destinations)
        {
            if (source.rank == destination.rank)
            {
                continue;
            }
            bool reachableFromSource = false;
            for (const Attachment& attachment : source.attachments)
            {
                if (SwitchPortIsUp(switches[attachment.switchIndex], attachment.switchPort)
                    && distance[attachment.switchIndex] != unreachable)
                {
                    reachableFromSource = true;
                    break;
                }
            }
            if (!reachableFromSource)
            {
                ++m_unreachableGpuPairs;
            }
        }
    }
    return m_unreachableGpuPairs == 0;
}

NodeContainer
GpuClusterTopologyHelper::BuildFullyConnected()
{
    NS_LOG_FUNCTION(this);

    // Create GPU nodes
    m_gpuNodes.Create(m_numGpus);

    // Create FabricEndpoint applications
    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    // Create a single switch for fully-connected topology
    m_switchNodes.Create(1);
    NvSwitchHelper switchHelper;
    Ptr<NetDevice> sw = switchHelper.Install(m_switchNodes.Get(0));
    Ptr<NvSwitch> nvSwitch = DynamicCast<NvSwitch>(sw);

    // Create point-to-point helper for links
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>",
                 "MaxSize",
                 StringValue("100000p"));

    // Track MAC addresses and switch port assignments for multi-link routing
    std::vector<std::vector<Mac48Address>> gpuMacs(m_numGpus);
    std::vector<std::vector<uint32_t>> gpuSwitchPorts(m_numGpus);

    // Connect each GPU to the switch (potentially with multiple links)
    for (uint32_t i = 0; i < m_numGpus; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
        endpointHelper.SetRank(m_endpoints.Get(i), i);

        // Set device type if overridden
        auto it = m_deviceTypes.find(static_cast<uint16_t>(i));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));

        for (uint32_t link = 0; link < m_linksPerGpu; ++link)
        {
            NetDeviceContainer devPair = p2p.Install(m_gpuNodes.Get(i), m_switchNodes.Get(0));
            Ptr<NetDevice> gpuDev = devPair.Get(0);
            Ptr<NetDevice> swPort = devPair.Get(1);

            ep->AddNetDevice(gpuDev);

            if (nvSwitch)
            {
                uint32_t portIdx = nvSwitch->AddPort(DynamicCast<PointToPointNetDevice>(swPort));
                gpuMacs[i].push_back(Mac48Address::ConvertFrom(gpuDev->GetAddress()));
                gpuSwitchPorts[i].push_back(portIdx);

                // Attach degradation model to switch port if set
                if (m_linkDegradationModel)
                {
                    Ptr<LinkDegradationModel> portModel = MakePortModel(
                        "intra_node", "electrical", "NVLink", 0.0, 0.5);
                    nvSwitch->SetPortDegradationModel(portIdx, portModel);
                }
            }
        }
    }

    // Configure switch routing
    if (nvSwitch && m_linksPerGpu > 1)
    {
        // Multi-link: enable spray routing on switch
        nvSwitch->SetSprayRouting(true);
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            // Static route via first port for each GPU MAC
            nvSwitch->AddStaticRoute(gpuMacs[gpu][0], gpuSwitchPorts[gpu][0]);
            // Spray: all ports to this GPU
            nvSwitch->AddSprayPorts(gpuMacs[gpu][0], gpuSwitchPorts[gpu]);
        }
    }
    else if (nvSwitch)
    {
        // Single-link: simple static routing (original behavior)
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            nvSwitch->AddStaticRoute(gpuMacs[gpu][0], gpuSwitchPorts[gpu][0]);
        }
    }

    // Propagate FEC model and LLR to switch (NVLink FEC operates at link level)
    if (nvSwitch)
    {
        if (m_fecModel)
        {
            nvSwitch->SetFecModel(m_fecModel);
        }
        nvSwitch->SetLlrEnabled(m_switchLlrEnabled);
    }

    // Configure endpoint routing
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));

        if (m_linksPerGpu > 1)
        {
            // Multi-link: enable spray routing on endpoint
            ep->SetSprayingEnabled(true);
            ep->SetSprayChunkSize(m_sprayChunkSize);
        }

        // Set neighbor MACs and routing entries for all GPUs (including self for
        // switched topology where self-dest means "send to switch for reduction")
        for (uint32_t dest = 0; dest < m_numGpus; ++dest)
        {
            if (dest == gpu)
            {
                // Self-dest routing for switched topology: all links reach the switch
                if (m_linksPerGpu > 1)
                {
                    ep->SetNeighborMac(static_cast<uint16_t>(dest), gpuMacs[gpu][0]);
                    ep->SetRoutingEntry(static_cast<uint16_t>(dest), 0);
                    std::vector<uint32_t> routingDevices;
                    for (uint32_t link = 0; link < m_linksPerGpu; ++link)
                    {
                        routingDevices.push_back(link);
                    }
                    ep->SetRoutingDevices(static_cast<uint16_t>(dest), routingDevices);
                }
                continue;
            }

            // Neighbor MAC: use first link's MAC address of destination GPU
            ep->SetNeighborMac(static_cast<uint16_t>(dest), gpuMacs[dest][0]);

            if (m_linksPerGpu > 1)
            {
                // Spray: route to all links
                ep->SetRoutingEntry(static_cast<uint16_t>(dest), 0);
                std::vector<uint32_t> routingDevices;
                for (uint32_t link = 0; link < m_linksPerGpu; ++link)
                {
                    routingDevices.push_back(link);
                }
                ep->SetRoutingDevices(static_cast<uint16_t>(dest), routingDevices);
            }
            else
            {
                // Single link: direct route via device 0
                ep->SetRoutingEntry(static_cast<uint16_t>(dest), 0);
            }
        }
    }

    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::BuildFatTree(uint32_t radix)
{
    NS_LOG_FUNCTION(this << radix);

    // Calculate number of switches needed
    uint32_t numLeafSwitches = (m_numGpus + radix - 1) / radix;
    uint32_t numSpineSwitches = radix;

    // Create GPU nodes
    m_gpuNodes.Create(m_numGpus);

    // Create leaf switches
    m_switchNodes.Create(numLeafSwitches + numSpineSwitches);

    // Create FabricEndpoint applications
    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    // Create switch helper
    NvSwitchHelper switchHelper;

    // Install switches
    std::vector<Ptr<NetDevice>> leafSwitches;
    std::vector<Ptr<NetDevice>> spineSwitches;

    for (uint32_t i = 0; i < numLeafSwitches; ++i)
    {
        leafSwitches.push_back(switchHelper.Install(m_switchNodes.Get(i)));
    }

    for (uint32_t i = 0; i < numSpineSwitches; ++i)
    {
        spineSwitches.push_back(switchHelper.Install(m_switchNodes.Get(numLeafSwitches + i)));
    }

    // Track GPU MACs and per-switch port assignments for static routing
    std::vector<Mac48Address> gpuMacs(m_numGpus);
    // leafGpuPorts[leaf][localIdx] = port on leaf for GPU (localIdx within this leaf)
    std::vector<std::vector<uint32_t>> leafGpuPorts(numLeafSwitches);
    // leafSpinePorts[leaf][spine] = port on leaf for spine
    std::vector<std::vector<uint32_t>> leafSpinePorts(numLeafSwitches, std::vector<uint32_t>(numSpineSwitches));
    // spineLeafPorts[spine][leaf] = port on spine for leaf
    std::vector<std::vector<uint32_t>> spineLeafPorts(numSpineSwitches, std::vector<uint32_t>(numLeafSwitches));

    // Create point-to-point helper
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>",
                 "MaxSize",
                 StringValue("100000p"));

    // Connect GPUs to leaf switches
    for (uint32_t i = 0; i < m_numGpus; ++i)
    {
        uint32_t leafId = i / radix;
        uint32_t localIdx = i % radix;
        NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(i), m_switchNodes.Get(leafId));

        endpointHelper.AddNetDevice(m_endpoints.Get(i), devices.Get(0));
        uint32_t portIdx = switchHelper.AddPort(leafSwitches[leafId], devices.Get(1));
        if (leafGpuPorts[leafId].size() <= localIdx)
        {
            leafGpuPorts[leafId].resize(localIdx + 1);
        }
        leafGpuPorts[leafId][localIdx] = portIdx;
        gpuMacs[i] = Mac48Address::ConvertFrom(devices.Get(0)->GetAddress());
        endpointHelper.SetRank(m_endpoints.Get(i), i);

        // Set device type if overridden
        auto it = m_deviceTypes.find(static_cast<uint16_t>(i));
        if (it != m_deviceTypes.end())
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            ep->SetDeviceType(it->second);
        }

        // Set fabric type
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        }

        // Attach degradation model to leaf switch port if set
        if (m_linkDegradationModel)
        {
            Ptr<NvSwitch> nvsw = DynamicCast<NvSwitch>(leafSwitches[leafId]);
            if (nvsw)
            {
                Ptr<LinkDegradationModel> portModel = MakePortModel(
                    "intra_rack", "electrical", "NVLink", 0.0, 2.0);
                nvsw->SetPortDegradationModel(nvsw->GetNPorts() - 1, portModel);
            }
        }
    }

    // Connect leaf switches to spine switches
    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
        {
            NetDeviceContainer devices = p2p.Install(
                m_switchNodes.Get(leaf),
                m_switchNodes.Get(numLeafSwitches + spine));

            uint32_t leafPort = switchHelper.AddPort(leafSwitches[leaf], devices.Get(0));
            uint32_t spinePort = switchHelper.AddPort(spineSwitches[spine], devices.Get(1));
            leafSpinePorts[leaf][spine] = leafPort;
            spineLeafPorts[spine][leaf] = spinePort;

            // Attach degradation model to inter-switch ports if set
            if (m_linkDegradationModel)
            {
                Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
                Ptr<NvSwitch> spineSw = DynamicCast<NvSwitch>(spineSwitches[spine]);
                if (leafSw)
                {
                    Ptr<LinkDegradationModel> portModel = MakePortModel(
                        "inter_rack", m_interSwitchMedium, "ScaleUp", 0.0, 10.0);
                    leafSw->SetPortDegradationModel(leafSw->GetNPorts() - 1, portModel);
                }
                if (spineSw)
                {
                    Ptr<LinkDegradationModel> portModel = MakePortModel(
                        "inter_rack", m_interSwitchMedium, "ScaleUp", 0.0, 10.0);
                    spineSw->SetPortDegradationModel(spineSw->GetNPorts() - 1, portModel);
                }
            }
        }
    }

    // Static L2 routing on all switches (NvSwitch has no MAC learning)
    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
        if (!leafSw) continue;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t gpuLeaf = gpu / radix;
            if (gpuLeaf == leaf)
            {
                // Local GPU: route to GPU-facing port
                uint32_t localIdx = gpu % radix;
                leafSw->AddStaticRoute(gpuMacs[gpu], leafGpuPorts[leaf][localIdx]);
            }
            else
            {
                // Remote GPU: route to any spine (all spines connect to all leaves)
                leafSw->AddStaticRoute(gpuMacs[gpu], leafSpinePorts[leaf][0]);
            }
        }
        if (m_fecModel)
        {
            leafSw->SetFecModel(m_fecModel);
        }
        leafSw->SetFecOpticalOnly(m_fecOpticalOnly);
        leafSw->SetLlrEnabled(m_switchLlrEnabled);
    }
    for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
    {
        Ptr<NvSwitch> spineSw = DynamicCast<NvSwitch>(spineSwitches[spine]);
        if (!spineSw) continue;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t gpuLeaf = gpu / radix;
            spineSw->AddStaticRoute(gpuMacs[gpu], spineLeafPorts[spine][gpuLeaf]);
        }
        if (m_fecModel)
        {
            spineSw->SetFecModel(m_fecModel);
        }
        spineSw->SetFecOpticalOnly(m_fecOpticalOnly);
        spineSw->SetLlrEnabled(m_switchLlrEnabled);
    }

    // Set endpoint neighbor MACs and routing so each GPU knows the dest MAC and device for every other GPU
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        if (!ep) continue;
        for (uint32_t dest = 0; dest < m_numGpus; ++dest)
        {
            if (dest == gpu) continue;
            ep->SetNeighborMac(static_cast<uint16_t>(dest), gpuMacs[dest]);
            // Single NetDevice per GPU — all traffic out device 0
            ep->SetRoutingEntry(static_cast<uint16_t>(dest), 0);
        }
    }

    AttachContentionModelToEndpoints();

    return NodeContainer(m_gpuNodes, m_switchNodes);
}

NodeContainer
GpuClusterTopologyHelper::BuildRailOptimizedFatTree(uint32_t numRails,
                                                    uint32_t nodesPerLeaf,
                                                    uint32_t numSpineSwitches,
                                                    uint32_t linksPerLeafSpine,
                                                    uint32_t numCoreSwitches)
{
    NS_LOG_FUNCTION(this << numRails << nodesPerLeaf << numSpineSwitches
                         << linksPerLeafSpine << numCoreSwitches);
    NS_ABORT_MSG_IF(numRails == 0, "Rail-optimized fat tree requires at least one rail");
    NS_ABORT_MSG_IF(nodesPerLeaf == 0,
                    "Rail-optimized fat tree requires at least one enclosure per leaf");
    NS_ABORT_MSG_IF(numSpineSwitches == 0,
                    "Rail-optimized fat tree requires at least one spine");
    NS_ABORT_MSG_IF(linksPerLeafSpine == 0,
                    "Rail-optimized fat tree requires at least one leaf-spine link");
    NS_ABORT_MSG_IF(m_numGpus % numRails != 0,
                    "GPU count must be divisible by the number of rails");
    NS_ABORT_MSG_IF(m_linksPerGpu != 1,
                    "Rail-optimized fat tree exposes one fabric interface per GPU");

    const uint32_t numEnclosures = m_numGpus / numRails;
    const uint32_t leavesPerRail =
        (numEnclosures + nodesPerLeaf - 1) / nodesPerLeaf;
    const uint32_t numLeafSwitches = numRails * leavesPerRail;
    const bool hasCoreTier = numCoreSwitches > 0;
    constexpr uint32_t switchesPerGroup = 32;

    if (hasCoreTier)
    {
        NS_ABORT_MSG_IF(numLeafSwitches % 64 != 0,
                        "Rail fat-tree core tier requires a leaf count divisible by 64");
        NS_ABORT_MSG_IF(numSpineSwitches != numLeafSwitches,
                        "Rail fat-tree core tier requires equal leaf and spine counts");
        NS_ABORT_MSG_IF(numCoreSwitches * 2 != numLeafSwitches,
                        "Rail fat-tree core tier requires one core per two leaves");
        NS_ABORT_MSG_IF(linksPerLeafSpine != 1,
                        "Rail fat-tree core tier uses one link per leaf-spine pair");
        NS_ABORT_MSG_IF(numCoreSwitches % switchesPerGroup != 0,
                        "Rail fat-tree core count must be divisible by 32");
        const uint32_t numPlanes = numCoreSwitches / switchesPerGroup;
        NS_ABORT_MSG_IF(numPlanes == 0 || switchesPerGroup % numPlanes != 0,
                        "Rail fat-tree core planes must divide the 32 spines in each group");
    }

    m_gpuNodes.Create(m_numGpus);
    m_switchNodes.Create(numLeafSwitches + numSpineSwitches + numCoreSwitches);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);
    NvSwitchHelper switchHelper;

    std::vector<Ptr<NetDevice>> leafSwitches;
    std::vector<Ptr<NetDevice>> spineSwitches;
    std::vector<Ptr<NetDevice>> coreSwitches;
    leafSwitches.reserve(numLeafSwitches);
    spineSwitches.reserve(numSpineSwitches);
    coreSwitches.reserve(numCoreSwitches);
    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        leafSwitches.push_back(switchHelper.Install(m_switchNodes.Get(leaf)));
    }
    for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
    {
        spineSwitches.push_back(
            switchHelper.Install(m_switchNodes.Get(numLeafSwitches + spine)));
    }
    for (uint32_t core = 0; core < numCoreSwitches; ++core)
    {
        coreSwitches.push_back(switchHelper.Install(
            m_switchNodes.Get(numLeafSwitches + numSpineSwitches + core)));
    }

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>",
                 "MaxSize",
                 StringValue("100000p"));

    std::vector<Mac48Address> gpuMacs(m_numGpus);
    std::vector<uint32_t> gpuLeaves(m_numGpus);
    std::vector<std::vector<uint32_t>> leafGpuPorts(
        numLeafSwitches, std::vector<uint32_t>(m_numGpus, 0));
    std::vector<std::vector<std::vector<uint32_t>>> leafSpinePorts(
        numLeafSwitches,
        std::vector<std::vector<uint32_t>>(numSpineSwitches));
    std::vector<std::vector<std::vector<uint32_t>>> spineLeafPorts(
        numSpineSwitches,
        std::vector<std::vector<uint32_t>>(numLeafSwitches));
    std::vector<std::vector<std::vector<uint32_t>>> spineCorePorts(
        numSpineSwitches,
        std::vector<std::vector<uint32_t>>(numCoreSwitches));
    std::vector<std::vector<std::vector<uint32_t>>> coreSpinePorts(
        numCoreSwitches,
        std::vector<std::vector<uint32_t>>(numSpineSwitches));

    const bool optical = m_interSwitchMedium == "optical";
    constexpr double accessDistance = 2.5;

    // GPU r in every enclosure joins rail r.  Consecutive groups of
    // nodesPerLeaf enclosures share one leaf on that rail.
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        const uint32_t enclosure = gpu / numRails;
        const uint32_t rail = gpu % numRails;
        const uint32_t segment = enclosure / nodesPerLeaf;
        const uint32_t leaf = rail * leavesPerRail + segment;
        gpuLeaves[gpu] = leaf;

        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        endpointHelper.SetRank(m_endpoints.Get(gpu), gpu);
        ep->SetNodeId(leaf);
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        auto type = m_deviceTypes.find(static_cast<uint16_t>(gpu));
        if (type != m_deviceTypes.end())
        {
            ep->SetDeviceType(type->second);
        }

        NetDeviceContainer devices = p2p.Install(
            m_gpuNodes.Get(gpu), m_switchNodes.Get(leaf));
        ep->AddNetDevice(devices.Get(0));
        const uint32_t port = switchHelper.AddPort(leafSwitches[leaf], devices.Get(1));
        leafGpuPorts[leaf][gpu] = port;
        gpuMacs[gpu] = Mac48Address::ConvertFrom(devices.Get(0)->GetAddress());

        if (m_linkDegradationModel)
        {
            Ptr<LinkDegradationModel> model = MakePortModel(
                "intra_rack", "electrical", "ScaleUp", 0.0, accessDistance);
            Ptr<NvSwitch> leafSwitch = DynamicCast<NvSwitch>(leafSwitches[leaf]);
            leafSwitch->SetPortDegradationModel(port, model);
            ep->SetLinkDegradationModel(MakePortModel(
                "intra_rack", "electrical", "ScaleUp", 0.0, accessDistance));
        }
    }

    // Up to 2048 GPUs, every leaf reaches every spine.  Parallel links retain
    // 32 uplinks per leaf as the spine count changes.  Larger systems use
    // groups of 32 leaves and 32 spines, followed by a core tier.
    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        const uint32_t firstSpine = hasCoreTier
            ? (leaf / switchesPerGroup) * switchesPerGroup
            : 0;
        const uint32_t lastSpine = hasCoreTier
            ? firstSpine + switchesPerGroup
            : numSpineSwitches;
        for (uint32_t spine = firstSpine; spine < lastSpine; ++spine)
        {
            for (uint32_t link = 0; link < linksPerLeafSpine; ++link)
            {
                NetDeviceContainer devices = p2p.Install(
                    m_switchNodes.Get(leaf),
                    m_switchNodes.Get(numLeafSwitches + spine));
                const uint32_t leafPort =
                    switchHelper.AddPort(leafSwitches[leaf], devices.Get(0));
                const uint32_t spinePort =
                    switchHelper.AddPort(spineSwitches[spine], devices.Get(1));
                leafSpinePorts[leaf][spine].push_back(leafPort);
                spineLeafPorts[spine][leaf].push_back(spinePort);

                if (m_linkDegradationModel)
                {
                    const double distance = optical ? 20.0 : 5.0;
                    Ptr<NvSwitch> leafSwitch = DynamicCast<NvSwitch>(leafSwitches[leaf]);
                    Ptr<NvSwitch> spineSwitch = DynamicCast<NvSwitch>(spineSwitches[spine]);
                    leafSwitch->SetPortDegradationModel(
                        leafPort,
                        MakePortModel("inter_row", m_interSwitchMedium,
                                      "ScaleUp", 0.0, distance));
                    spineSwitch->SetPortDegradationModel(
                        spinePort,
                        MakePortModel("inter_row", m_interSwitchMedium,
                                      "ScaleUp", 0.0, distance));
                }
            }
        }
    }

    if (hasCoreTier)
    {
        const uint32_t numPlanes = numCoreSwitches / switchesPerGroup;
        for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
        {
            const uint32_t localSpine = spine % switchesPerGroup;
            const uint32_t plane = localSpine % numPlanes;
            const uint32_t firstCore = plane * switchesPerGroup;
            for (uint32_t core = firstCore;
                 core < firstCore + switchesPerGroup;
                 ++core)
            {
                NetDeviceContainer devices = p2p.Install(
                    m_switchNodes.Get(numLeafSwitches + spine),
                    m_switchNodes.Get(numLeafSwitches + numSpineSwitches + core));
                const uint32_t spinePort =
                    switchHelper.AddPort(spineSwitches[spine], devices.Get(0));
                const uint32_t corePort =
                    switchHelper.AddPort(coreSwitches[core], devices.Get(1));
                spineCorePorts[spine][core].push_back(spinePort);
                coreSpinePorts[core][spine].push_back(corePort);

                if (m_linkDegradationModel)
                {
                    const double distance = optical ? 20.0 : 5.0;
                    Ptr<NvSwitch> spineSwitch = DynamicCast<NvSwitch>(spineSwitches[spine]);
                    Ptr<NvSwitch> coreSwitch = DynamicCast<NvSwitch>(coreSwitches[core]);
                    spineSwitch->SetPortDegradationModel(
                        spinePort,
                        MakePortModel("inter_row", m_interSwitchMedium,
                                      "ScaleUp", 0.0, distance));
                    coreSwitch->SetPortDegradationModel(
                        corePort,
                        MakePortModel("inter_row", m_interSwitchMedium,
                                      "ScaleUp", 0.0, distance));
                }
            }
        }
    }

    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        Ptr<NvSwitch> leafSwitch = DynamicCast<NvSwitch>(leafSwitches[leaf]);
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            if (gpuLeaves[gpu] == leaf)
            {
                leafSwitch->AddStaticRoute(gpuMacs[gpu], leafGpuPorts[leaf][gpu]);
                continue;
            }
            std::vector<uint32_t> uplinks;
            for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
            {
                uplinks.insert(uplinks.end(),
                               leafSpinePorts[leaf][spine].begin(),
                               leafSpinePorts[leaf][spine].end());
            }
            leafSwitch->AddStaticRoute(gpuMacs[gpu], uplinks.front());
            leafSwitch->AddSprayPorts(gpuMacs[gpu], uplinks);
        }
        leafSwitch->SetSprayRouting(true);
        if (m_fecModel)
        {
            leafSwitch->SetFecModel(m_fecModel);
        }
        leafSwitch->SetFecOpticalOnly(m_fecOpticalOnly);
        leafSwitch->SetLlrEnabled(m_switchLlrEnabled);
    }

    for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
    {
        Ptr<NvSwitch> spineSwitch = DynamicCast<NvSwitch>(spineSwitches[spine]);
        const uint32_t spineGroup = spine / switchesPerGroup;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            const uint32_t destinationLeaf = gpuLeaves[gpu];
            const uint32_t destinationGroup = destinationLeaf / switchesPerGroup;
            if (!hasCoreTier || destinationGroup == spineGroup)
            {
                const std::vector<uint32_t>& downlinks =
                    spineLeafPorts[spine][destinationLeaf];
                spineSwitch->AddStaticRoute(gpuMacs[gpu], downlinks.front());
                spineSwitch->AddSprayPorts(gpuMacs[gpu], downlinks);
                continue;
            }

            std::vector<uint32_t> uplinks;
            for (uint32_t core = 0; core < numCoreSwitches; ++core)
            {
                uplinks.insert(uplinks.end(),
                               spineCorePorts[spine][core].begin(),
                               spineCorePorts[spine][core].end());
            }
            spineSwitch->AddStaticRoute(gpuMacs[gpu], uplinks.front());
            spineSwitch->AddSprayPorts(gpuMacs[gpu], uplinks);
        }
        spineSwitch->SetSprayRouting(true);
        if (m_fecModel)
        {
            spineSwitch->SetFecModel(m_fecModel);
        }
        spineSwitch->SetFecOpticalOnly(m_fecOpticalOnly);
        spineSwitch->SetLlrEnabled(m_switchLlrEnabled);
    }

    for (uint32_t core = 0; core < numCoreSwitches; ++core)
    {
        Ptr<NvSwitch> coreSwitch = DynamicCast<NvSwitch>(coreSwitches[core]);
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            const uint32_t destinationGroup =
                gpuLeaves[gpu] / switchesPerGroup;
            const uint32_t firstSpine = destinationGroup * switchesPerGroup;
            std::vector<uint32_t> downlinks;
            for (uint32_t spine = firstSpine;
                 spine < firstSpine + switchesPerGroup;
                 ++spine)
            {
                downlinks.insert(downlinks.end(),
                                 coreSpinePorts[core][spine].begin(),
                                 coreSpinePorts[core][spine].end());
            }
            coreSwitch->AddStaticRoute(gpuMacs[gpu], downlinks.front());
            coreSwitch->AddSprayPorts(gpuMacs[gpu], downlinks);
        }
        coreSwitch->SetSprayRouting(true);
        if (m_fecModel)
        {
            coreSwitch->SetFecModel(m_fecModel);
        }
        coreSwitch->SetFecOpticalOnly(m_fecOpticalOnly);
        coreSwitch->SetLlrEnabled(m_switchLlrEnabled);
    }

    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        for (uint32_t destination = 0; destination < m_numGpus; ++destination)
        {
            if (destination == gpu)
            {
                continue;
            }
            ep->SetNeighborMac(static_cast<uint16_t>(destination), gpuMacs[destination]);
            ep->SetRoutingEntry(static_cast<uint16_t>(destination), 0);
        }
    }

    AttachContentionModelToEndpoints();
    return NodeContainer(m_gpuNodes, m_switchNodes);
}

NodeContainer
GpuClusterTopologyHelper::BuildRing()
{
    NS_LOG_FUNCTION(this);

    NS_ABORT_MSG_IF(m_numGpus < 2, "Ring topology requires at least 2 nodes");

    // Create nodes
    m_gpuNodes.Create(m_numGpus);

    // Create FabricEndpoint applications
    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    // Set ranks and device types
    for (uint32_t i = 0; i < m_numGpus; ++i)
    {
        endpointHelper.SetRank(m_endpoints.Get(i), i);
        auto it = m_deviceTypes.find(static_cast<uint16_t>(i));
        if (it != m_deviceTypes.end())
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            ep->SetDeviceType(it->second);
        }
        // Set fabric type
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        }
    }

    // No switches in ring topology
    m_switchNodes = NodeContainer();

    // Create point-to-point helper for links
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // Track which logical device index connects to which neighbor for each node.
    // cwLogicalDev[node] = logical device index connecting to clockwise neighbor (node+1)%N
    // ccwLogicalDev[node] = logical device index connecting to counter-clockwise neighbor (node-1+N)%N
    std::vector<uint32_t> cwLogicalDev(m_numGpus);
    std::vector<uint32_t> ccwLogicalDev(m_numGpus);

    // Track lane groups: for each endpoint, map logicalDevIdx -> physical device indices
    // LaneGroup per node per direction
    std::vector<std::vector<std::vector<uint32_t>>> cwLaneGroups(m_numGpus);
    std::vector<std::vector<std::vector<uint32_t>>> ccwLaneGroups(m_numGpus);

    // Connect each node to its right (clockwise) neighbor in ring
    for (uint32_t i = 0; i < m_numGpus; ++i)
    {
        uint32_t right = (i + 1) % m_numGpus;
        Ptr<FabricEndpoint> epI = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
        Ptr<FabricEndpoint> epRight = DynamicCast<FabricEndpoint>(m_endpoints.Get(right));

        // Record the logical device index (first lane's device index)
        uint32_t cwLogicalDevIdx = epI->GetNNetDevices();
        uint32_t ccwLogicalDevIdx = epRight->GetNNetDevices();
        cwLogicalDev[i] = cwLogicalDevIdx;
        ccwLogicalDev[right] = ccwLogicalDevIdx;

        std::vector<uint32_t> cwPhysicalDevIndices;
        std::vector<uint32_t> ccwPhysicalDevIndices;

        // Create m_numLanes p2p pairs for this ring edge
        for (uint32_t lane = 0; lane < m_numLanes; ++lane)
        {
            NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(i), m_gpuNodes.Get(right));

            Ptr<PointToPointNetDevice> devI = DynamicCast<PointToPointNetDevice>(devices.Get(0));
            Ptr<PointToPointNetDevice> devRight = DynamicCast<PointToPointNetDevice>(devices.Get(1));

            // Add NetDevice to endpoints
            epI->AddNetDevice(devices.Get(0));
            epRight->AddNetDevice(devices.Get(1));

            // Track physical device indices
            cwPhysicalDevIndices.push_back(epI->GetNNetDevices() - 1);
            ccwPhysicalDevIndices.push_back(epRight->GetNNetDevices() - 1);

            // Set neighbor MAC for first lane only (all lanes share same destRank→MAC)
            if (lane == 0)
            {
                Mac48Address addrI = Mac48Address::ConvertFrom(devI->GetAddress());
                Mac48Address addrRight = Mac48Address::ConvertFrom(devRight->GetAddress());
                epI->SetNeighborMac(static_cast<uint16_t>(right), addrRight);
                epRight->SetNeighborMac(static_cast<uint16_t>(i), addrI);
            }
        }

        // Register lane groups for sub-device spraying
        if (m_numLanes > 1)
        {
            epI->SetLaneGroup(cwLogicalDevIdx, cwPhysicalDevIndices);
            epRight->SetLaneGroup(ccwLogicalDevIdx, ccwPhysicalDevIndices);
        }
    }

    // Set numLanes on all endpoints and populate routing tables
    for (uint32_t src = 0; src < m_numGpus; ++src)
    {
        Ptr<FabricEndpoint> epSrc = DynamicCast<FabricEndpoint>(m_endpoints.Get(src));
        epSrc->SetNumLanes(m_numLanes);
        epSrc->ClearRoutingTable();

        for (uint32_t dst = 0; dst < m_numGpus; ++dst)
        {
            if (src == dst)
            {
                continue;
            }

            // Compute ring distances
            uint32_t cwDist = (dst > src) ? (dst - src) : (m_numGpus - src + dst);
            uint32_t ccwDist = m_numGpus - cwDist;

            // Route via the shorter path (use logical device index)
            if (cwDist <= ccwDist)
            {
                epSrc->SetRoutingEntry(static_cast<uint16_t>(dst), cwLogicalDev[src]);
            }
            else
            {
                epSrc->SetRoutingEntry(static_cast<uint16_t>(dst), ccwLogicalDev[src]);
            }
        }

        // Attach degradation model if set
        if (m_linkDegradationModel)
        {
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            epSrc->SetLinkDegradationModel(portModel);
        }
    }

    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::BuildFullMesh()
{
    NS_LOG_FUNCTION(this);

    NS_ABORT_MSG_IF(m_numGpus < 2, "Full-mesh topology requires at least 2 nodes");

    // Create nodes
    m_gpuNodes.Create(m_numGpus);

    // Create FabricEndpoint applications
    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    // Set ranks and device types
    for (uint32_t i = 0; i < m_numGpus; ++i)
    {
        endpointHelper.SetRank(m_endpoints.Get(i), i);
        auto it = m_deviceTypes.find(static_cast<uint16_t>(i));
        if (it != m_deviceTypes.end())
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            ep->SetDeviceType(it->second);
        }
        // Set fabric type
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        }
    }

    // No switches in full-mesh topology
    m_switchNodes = NodeContainer();

    // Create point-to-point helper for links
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // Connect every pair of nodes with direct links (m_numLanes per edge)
    for (uint32_t i = 0; i < m_numGpus; ++i)
    {
        for (uint32_t j = i + 1; j < m_numGpus; ++j)
        {
            Ptr<FabricEndpoint> epI = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            Ptr<FabricEndpoint> epJ = DynamicCast<FabricEndpoint>(m_endpoints.Get(j));

            // Record logical device index (first lane)
            uint32_t logicalDevIdxI = epI->GetNNetDevices();
            uint32_t logicalDevIdxJ = epJ->GetNNetDevices();

            std::vector<uint32_t> physicalDevIndicesI;
            std::vector<uint32_t> physicalDevIndicesJ;

            for (uint32_t lane = 0; lane < m_numLanes; ++lane)
            {
                NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(i), m_gpuNodes.Get(j));

                Ptr<PointToPointNetDevice> devI = DynamicCast<PointToPointNetDevice>(devices.Get(0));
                Ptr<PointToPointNetDevice> devJ = DynamicCast<PointToPointNetDevice>(devices.Get(1));

                epI->AddNetDevice(devices.Get(0));
                epJ->AddNetDevice(devices.Get(1));

                physicalDevIndicesI.push_back(epI->GetNNetDevices() - 1);
                physicalDevIndicesJ.push_back(epJ->GetNNetDevices() - 1);

                if (lane == 0)
                {
                    Mac48Address addrJ = Mac48Address::ConvertFrom(devJ->GetAddress());
                    Mac48Address addrI = Mac48Address::ConvertFrom(devI->GetAddress());
                    epI->SetNeighborMac(static_cast<uint16_t>(j), addrJ);
                    epJ->SetNeighborMac(static_cast<uint16_t>(i), addrI);
                }
            }

            // Register lane groups for sub-device spraying
            if (m_numLanes > 1)
            {
                epI->SetLaneGroup(logicalDevIdxI, physicalDevIndicesI);
                epJ->SetLaneGroup(logicalDevIdxJ, physicalDevIndicesJ);
            }

            // Set routing entries using logical device index
            epI->SetRoutingEntry(static_cast<uint16_t>(j), logicalDevIdxI);
            epJ->SetRoutingEntry(static_cast<uint16_t>(i), logicalDevIdxJ);
        }
    }

    // Set numLanes and attach degradation model
    for (uint32_t i = 0; i < m_numGpus; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
        ep->SetNumLanes(m_numLanes);

        if (m_linkDegradationModel)
        {
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            ep->SetLinkDegradationModel(portModel);
        }
    }

    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::BuildNvl72(uint32_t numSwitchPlanes, uint32_t gpusPerGroup)
{
    NS_LOG_FUNCTION(this << numSwitchPlanes << gpusPerGroup);

    NS_ABORT_MSG_IF(m_numGpus < 2, "NVL72 topology requires at least 2 GPUs");
    NS_ABORT_MSG_IF(m_linksPerGpu < 1, "NVL72 requires linksPerGpu >= 1");
    // linksPerGpu must equal numSwitchPlanes for true NVL72 (1 link per switch plane)
    // But allow fewer for testing; the routing will just use available planes
    uint32_t effectivePlanes = std::min(m_linksPerGpu, numSwitchPlanes);

    if (gpusPerGroup == 0)
    {
        gpusPerGroup = m_numGpus / 6;  // Default: 6 groups of 12 for 72 GPUs
        if (gpusPerGroup == 0) gpusPerGroup = m_numGpus;
    }
    NS_ABORT_MSG_IF(m_numGpus % gpusPerGroup != 0,
                     "NVL72 requires GPUs evenly divisible by gpusPerGroup");

    uint32_t numGroups = m_numGpus / gpusPerGroup;
    (void)numGroups;

    // Create GPU nodes
    m_gpuNodes.Create(m_numGpus);

    // Create switch-plane nodes (one per NVSwitch ASIC that we'll actually connect to)
    m_switchNodes.Create(effectivePlanes);

    // Create FabricEndpoint applications
    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    NvSwitchHelper switchHelper;
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>",
                 "MaxSize",
                 StringValue("100000p"));

    // Install switches
    std::vector<Ptr<NetDevice>> switches;
    for (uint32_t i = 0; i < effectivePlanes; ++i)
    {
        switches.push_back(switchHelper.Install(m_switchNodes.Get(i)));
    }

    // Track MAC addresses per GPU (use first link's MAC as canonical)
    std::vector<Mac48Address> gpuMacs(m_numGpus);
    // Track per-switch port assignments for each GPU
    // switchGpuPorts[switch][gpu] = list of port numbers
    std::vector<std::vector<std::vector<uint32_t>>> switchGpuPorts(effectivePlanes);

    // Connect each GPU to each switch plane with one link
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        endpointHelper.SetRank(m_endpoints.Get(gpu), gpu);
        auto it = m_deviceTypes.find(static_cast<uint16_t>(gpu));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));

        if (effectivePlanes > 1)
        {
            ep->SetSprayingEnabled(true);
            ep->SetSprayChunkSize(m_sprayChunkSize);
        }

        std::vector<uint32_t> portsForThisGpu;

        for (uint32_t sw = 0; sw < effectivePlanes; ++sw)
        {
            NetDeviceContainer devPair = p2p.Install(m_gpuNodes.Get(gpu), m_switchNodes.Get(sw));
            ep->AddNetDevice(devPair.Get(0));
            uint32_t portNum = switchHelper.AddPort(switches[sw], devPair.Get(1));
            portsForThisGpu.push_back(portNum);

            // Attach degradation model to switch port if set
            if (m_linkDegradationModel)
            {
                Ptr<NvSwitch> nvSw = DynamicCast<NvSwitch>(switches[sw]);
                if (nvSw)
                {
                    Ptr<LinkDegradationModel> portModel = MakePortModel(
                        "intra_node", "electrical", "NVLink", 0.0, 0.5);
                    nvSw->SetPortDegradationModel(portNum, portModel);
                }
            }

            if (sw == 0)
            {
                gpuMacs[gpu] = Mac48Address::ConvertFrom(devPair.Get(0)->GetAddress());
            }

            switchGpuPorts[sw].push_back({portNum});
        }

        // Set leaf-group membership for hierarchical collectives
        uint32_t groupId = gpu / gpusPerGroup;
        uint16_t groupBaseRank = static_cast<uint16_t>(groupId * gpusPerGroup);
        ep->SetNodeId(groupId);
        ep->SetLocalRankRange(groupBaseRank, static_cast<uint16_t>(gpusPerGroup));
    }

    // Set endpoint routing: each destination reachable via ALL switch planes
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));

        for (uint32_t dest = 0; dest < m_numGpus; ++dest)
        {
            ep->SetNeighborMac(static_cast<uint16_t>(dest), gpuMacs[dest]);

            if (dest == gpu)
            {
                // Self-dest: all devices for spray
                if (effectivePlanes > 1)
                {
                    ep->SetRoutingEntry(static_cast<uint16_t>(dest), 0);
                    std::vector<uint32_t> routingDevices;
                    for (uint32_t d = 0; d < effectivePlanes; ++d)
                    {
                        routingDevices.push_back(d);
                    }
                    ep->SetRoutingDevices(static_cast<uint16_t>(dest), routingDevices);
                }
                continue;
            }

            // All switch planes can reach any destination — spray across all planes
            if (effectivePlanes > 1)
            {
                ep->SetRoutingEntry(static_cast<uint16_t>(dest), 0);
                std::vector<uint32_t> routingDevices;
                for (uint32_t d = 0; d < effectivePlanes; ++d)
                {
                    routingDevices.push_back(d);
                }
                ep->SetRoutingDevices(static_cast<uint16_t>(dest), routingDevices);
            }
            else
            {
                ep->SetRoutingEntry(static_cast<uint16_t>(dest), 0);
            }
        }
    }

    // Configure switch routing: each switch knows all GPUs and routes to the
    // correct port. Since every GPU is connected to every switch, each switch
    // can directly forward to any GPU without going through another switch.
    for (uint32_t sw = 0; sw < effectivePlanes; ++sw)
    {
        Ptr<NvSwitch> nvSw = DynamicCast<NvSwitch>(switches[sw]);
        if (!nvSw) continue;

        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            nvSw->AddStaticRoute(gpuMacs[gpu], switchGpuPorts[sw][gpu][0]);
        }

        nvSw->SetSprayRouting(true);

        // Propagate FEC model to switch (NVLink FEC operates at link level)
        if (m_fecModel)
        {
            nvSw->SetFecModel(m_fecModel);
        }

        // Enable LLR on switch ports when endpoint LLR is enabled
        // (NVSwitch NVLink ports detect CRC/FEC failure and send NACK back)
        nvSw->SetLlrEnabled(m_switchLlrEnabled);
    }

    AttachContentionModelToEndpoints();

    return NodeContainer(m_gpuNodes, m_switchNodes);
}

NodeContainer
GpuClusterTopologyHelper::BuildLeafSpine(uint32_t numLeafSwitches, uint32_t numSpineSwitches)
{
    NS_LOG_FUNCTION(this << numLeafSwitches << numSpineSwitches);

    NS_ABORT_MSG_IF(m_numGpus < 1, "Leaf-spine topology requires at least 1 GPU");
    NS_ABORT_MSG_IF(numLeafSwitches < 1, "Leaf-spine topology requires at least 1 leaf switch");
    NS_ABORT_MSG_IF(numSpineSwitches < 1, "Leaf-spine topology requires at least 1 spine switch");
    NS_ABORT_MSG_IF(m_numGpus < numLeafSwitches,
                     "Leaf-spine requires at least as many GPUs as leaf switches");
    NS_ABORT_MSG_IF(m_numGpus % numLeafSwitches != 0,
                     "Leaf-spine requires GPUs evenly divisible by leaf switches");
    NS_ABORT_MSG_IF(m_linksPerGpu < 1, "Leaf-spine requires at least one link per GPU");

    uint32_t gpusPerLeaf = m_numGpus / numLeafSwitches;

    // Create GPU nodes
    m_gpuNodes.Create(m_numGpus);

    // Create leaf + spine switch nodes
    m_switchNodes.Create(numLeafSwitches + numSpineSwitches);

    // Create FabricEndpoint applications
    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    NvSwitchHelper switchHelper;
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // Install leaf switches
    std::vector<Ptr<NetDevice>> leafSwitches;
    for (uint32_t i = 0; i < numLeafSwitches; ++i)
    {
        leafSwitches.push_back(switchHelper.Install(m_switchNodes.Get(i)));
    }

    // Install spine switches
    std::vector<Ptr<NetDevice>> spineSwitches;
    for (uint32_t i = 0; i < numSpineSwitches; ++i)
    {
        spineSwitches.push_back(switchHelper.Install(m_switchNodes.Get(numLeafSwitches + i)));
    }

    // Track MAC addresses per GPU
    std::vector<Mac48Address> gpuMacs(m_numGpus);

    // Track per-leaf switch port assignments for GPU MACs
    // leafGpuPort[leaf][localGpuIdx] = switch port number for that GPU on that leaf
    std::vector<std::vector<uint32_t>> leafGpuPorts(numLeafSwitches);
    // Track all ports (including multi-link) for spray routing
    std::vector<std::vector<std::vector<uint32_t>>> leafGpuAllPorts(numLeafSwitches);

    // Connect each GPU to its home leaf. Parallel GPU links terminate on the
    // same leaf; traffic to another leaf must traverse a spine switch.
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        endpointHelper.SetRank(m_endpoints.Get(gpu), gpu);
        auto it = m_deviceTypes.find(static_cast<uint16_t>(gpu));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));

        if (m_linksPerGpu > 1)
        {
            ep->SetSprayingEnabled(true);
            ep->SetSprayChunkSize(m_sprayChunkSize);
        }

        uint32_t leaf = gpu / gpusPerLeaf;
        std::vector<uint32_t> portsForThisGpu;
        for (uint32_t link = 0; link < m_linksPerGpu; ++link)
        {
            NetDeviceContainer devPair = p2p.Install(
                m_gpuNodes.Get(gpu), m_switchNodes.Get(leaf));
            ep->AddNetDevice(devPair.Get(0));
            uint32_t portNum = switchHelper.AddPort(leafSwitches[leaf], devPair.Get(1));
            portsForThisGpu.push_back(portNum);

            if (m_linkDegradationModel)
            {
                Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
                if (leafSw)
                {
                    leafSw->SetPortDegradationModel(
                        portNum,
                        MakePortModel("intra_rack", "electrical", "NVLink", 0.0, 2.0));
                }
            }

            if (link == 0)
            {
                gpuMacs[gpu] = Mac48Address::ConvertFrom(devPair.Get(0)->GetAddress());
            }
        }
        leafGpuPorts[leaf].push_back(portsForThisGpu[0]);
        leafGpuAllPorts[leaf].push_back(portsForThisGpu);

        // Set leaf-group membership for hierarchical collectives
        uint32_t leafGroup = gpu / gpusPerLeaf;
        uint16_t leafBaseRank = static_cast<uint16_t>(leafGroup * gpusPerLeaf);
        ep->SetNodeId(leafGroup);
        ep->SetLocalRankRange(leafBaseRank, static_cast<uint16_t>(gpusPerLeaf));
    }

    // Set endpoint routing and neighbor MACs after all gpuMacs are populated
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        // Every destination leaves through the source GPU's home leaf. The
        // leaf switch forwards local traffic directly and remote traffic to a
        // spine according to its static route.
        for (uint32_t dest = 0; dest < m_numGpus; ++dest)
        {
            if (dest == gpu) continue;
            if (m_linksPerGpu > 1)
            {
                std::vector<uint32_t> devices;
                for (uint32_t link = 0; link < m_linksPerGpu; ++link)
                {
                    devices.push_back(link);
                }
                ep->SetRoutingDevices(static_cast<uint16_t>(dest), devices);
            }
            else
            {
                ep->SetRoutingEntry(static_cast<uint16_t>(dest), 0);
            }
            ep->SetNeighborMac(static_cast<uint16_t>(dest), gpuMacs[dest]);
        }

        // Self-destination routing is needed by switch-assisted paths.
        if (m_linksPerGpu > 1)
        {
            uint32_t totalDevices = ep->GetNNetDevices();
            if (totalDevices > 1)
            {
                ep->SetNeighborMac(static_cast<uint16_t>(gpu), gpuMacs[gpu]);
                ep->SetRoutingEntry(static_cast<uint16_t>(gpu), 0);
                std::vector<uint32_t> routingDevices;
                for (uint32_t d = 0; d < totalDevices; ++d)
                {
                    routingDevices.push_back(d);
                }
                ep->SetRoutingDevices(static_cast<uint16_t>(gpu), routingDevices);
            }
        }
    }

    // Connect leaf switches to spine switches
    // Track spine port numbers per leaf switch
    std::vector<std::vector<uint32_t>> leafSpinePorts(numLeafSwitches);
    std::vector<std::vector<uint32_t>> spineLeafPorts(numSpineSwitches);

    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
        {
            NetDeviceContainer devPair = p2p.Install(
                m_switchNodes.Get(leaf),
                m_switchNodes.Get(numLeafSwitches + spine));
            uint32_t leafPort = switchHelper.AddPort(leafSwitches[leaf], devPair.Get(0));
            uint32_t spinePort = switchHelper.AddPort(spineSwitches[spine], devPair.Get(1));
            leafSpinePorts[leaf].push_back(leafPort);
            spineLeafPorts[spine].push_back(spinePort);

            if (m_linkDegradationModel)
            {
                const double distanceMeters = (m_interSwitchMedium == "optical") ? 10.0 : 3.0;
                Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
                Ptr<NvSwitch> spineSw = DynamicCast<NvSwitch>(spineSwitches[spine]);
                if (leafSw)
                {
                    leafSw->SetPortDegradationModel(
                        leafPort,
                        MakePortModel("inter_rack",
                                      m_interSwitchMedium,
                                      "ScaleUp",
                                      0.0,
                                      distanceMeters));
                }
                if (spineSw)
                {
                    spineSw->SetPortDegradationModel(
                        spinePort,
                        MakePortModel("inter_rack",
                                      m_interSwitchMedium,
                                      "ScaleUp",
                                      0.0,
                                      distanceMeters));
                }
            }
        }
    }

    // Configure leaf switch routing
    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
        if (!leafSw) continue;

        // Add static routes for local GPUs (GPU port on this leaf)
        uint32_t leafBaseGpu = leaf * gpusPerLeaf;
        for (uint32_t localIdx = 0; localIdx < gpusPerLeaf; ++localIdx)
        {
            uint32_t gpu = leafBaseGpu + localIdx;
            Mac48Address gpuMac = gpuMacs[gpu];
            // Static route via first port for this GPU
            leafSw->AddStaticRoute(gpuMac, leafGpuPorts[leaf][localIdx]);
            // Spray routing: all ports to this GPU
            if (m_linksPerGpu > 1)
            {
                leafSw->AddSprayPorts(gpuMac, leafGpuAllPorts[leaf][localIdx]);
            }
        }

        // Route non-local GPU MACs to spine ports (round-robin distribution across
        // all spine switches for balanced inter-leaf bandwidth utilization)
        uint32_t remoteGpuIdx = 0;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t gpuLeaf = gpu / gpusPerLeaf;
            if (gpuLeaf == leaf) continue;  // Local GPU, already routed
            uint32_t spinePortIdx = remoteGpuIdx % numSpineSwitches;
            leafSw->AddStaticRoute(gpuMacs[gpu], leafSpinePorts[leaf][spinePortIdx]);
            remoteGpuIdx++;
        }

        leafSw->SetSprayRouting(true);

        // Propagate FEC model and LLR to leaf switch
        if (m_fecModel)
        {
            leafSw->SetFecModel(m_fecModel);
        }
        leafSw->SetFecOpticalOnly(m_fecOpticalOnly);
        leafSw->SetLlrEnabled(m_switchLlrEnabled);
    }

    // Configure spine switch routing
    for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
    {
        Ptr<NvSwitch> spineSw = DynamicCast<NvSwitch>(spineSwitches[spine]);
        if (!spineSw) continue;

        // Route all GPU MACs to the correct leaf port
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t gpuLeaf = gpu / gpusPerLeaf;
            spineSw->AddStaticRoute(gpuMacs[gpu], spineLeafPorts[spine][gpuLeaf]);
        }

        spineSw->SetSprayRouting(true);

        // Propagate FEC model and LLR to spine switch
        if (m_fecModel)
        {
            spineSw->SetFecModel(m_fecModel);
        }
        spineSw->SetFecOpticalOnly(m_fecOpticalOnly);
        spineSw->SetLlrEnabled(m_switchLlrEnabled);
    }

    if (m_linkDegradationModel)
    {
        for (uint32_t i = 0; i < m_endpoints.GetN(); ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            ep->SetLinkDegradationModel(
                MakePortModel("intra_rack", "electrical", "NVLink", 0.0, 2.0));
        }
    }

    AttachContentionModelToEndpoints();

    return NodeContainer(m_gpuNodes, m_switchNodes);
}

NodeContainer
GpuClusterTopologyHelper::BuildTorus(uint32_t dimX, uint32_t dimY, uint32_t dimZ)
{
    NS_LOG_FUNCTION(this << dimX << dimY << dimZ);

    uint32_t numNodes = dimX * dimY * dimZ;
    NS_ABORT_MSG_IF(numNodes < 2, "Torus topology requires at least 2 nodes");

    m_numGpus = numNodes;
    m_gpuNodes.Create(numNodes);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);
    m_switchNodes = NodeContainer();

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // Assign ranks: node index = x * dimY * dimZ + y * dimZ + z
    for (uint32_t idx = 0; idx < numNodes; ++idx)
    {
        endpointHelper.SetRank(m_endpoints.Get(idx), idx);
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx));
        auto it = m_deviceTypes.find(static_cast<uint16_t>(idx));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
    }

    auto indexFromCoord = [&](uint32_t x, uint32_t y, uint32_t z) {
        return x * dimY * dimZ + y * dimZ + z;
    };

    // Connect links in each dimension with wrap-around
    // Helper lambda to install a link between two nodes
    auto installLink = [&](uint32_t idx1, uint32_t idx2) {
        NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(idx1), m_gpuNodes.Get(idx2));
        Ptr<FabricEndpoint> ep1 = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx1));
        Ptr<FabricEndpoint> ep2 = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx2));

        uint32_t devIdx1 = ep1->GetNNetDevices();
        uint32_t devIdx2 = ep2->GetNNetDevices();

        ep1->AddNetDevice(devices.Get(0));
        ep2->AddNetDevice(devices.Get(1));

        Mac48Address addr1 = Mac48Address::ConvertFrom(devices.Get(0)->GetAddress());
        Mac48Address addr2 = Mac48Address::ConvertFrom(devices.Get(1)->GetAddress());

        // Set routing for direct neighbor
        ep1->SetRoutingEntry(static_cast<uint16_t>(idx2), devIdx1);
        ep1->SetNeighborMac(static_cast<uint16_t>(idx2), addr2);

        ep2->SetRoutingEntry(static_cast<uint16_t>(idx1), devIdx2);
        ep2->SetNeighborMac(static_cast<uint16_t>(idx1), addr1);
    };

    // X dimension
    for (uint32_t y = 0; y < dimY; ++y)
    {
        for (uint32_t z = 0; z < dimZ; ++z)
        {
            for (uint32_t x = 0; x < dimX; ++x)
            {
                uint32_t idx1 = indexFromCoord(x, y, z);
                uint32_t idx2 = indexFromCoord((x + 1) % dimX, y, z);
                if (idx1 < idx2)
                {
                    installLink(idx1, idx2);
                }
            }
        }
    }

    // Y dimension
    if (dimY > 1)
    {
        for (uint32_t x = 0; x < dimX; ++x)
        {
            for (uint32_t z = 0; z < dimZ; ++z)
            {
                for (uint32_t y = 0; y < dimY; ++y)
                {
                    uint32_t idx1 = indexFromCoord(x, y, z);
                    uint32_t idx2 = indexFromCoord(x, (y + 1) % dimY, z);
                    if (idx1 < idx2)
                    {
                        installLink(idx1, idx2);
                    }
                }
            }
        }
    }

    // Z dimension
    if (dimZ > 1)
    {
        for (uint32_t x = 0; x < dimX; ++x)
        {
            for (uint32_t y = 0; y < dimY; ++y)
            {
                for (uint32_t z = 0; z < dimZ; ++z)
                {
                    uint32_t idx1 = indexFromCoord(x, y, z);
                    uint32_t idx2 = indexFromCoord(x, y, (z + 1) % dimZ);
                    if (idx1 < idx2)
                    {
                        installLink(idx1, idx2);
                    }
                }
            }
        }
    }

    // Populate dimension-order routing tables for multi-hop destinations
    for (uint32_t src = 0; src < numNodes; ++src)
    {
        Ptr<FabricEndpoint> epSrc = DynamicCast<FabricEndpoint>(m_endpoints.Get(src));

        uint32_t srcX = src / (dimY * dimZ);
        uint32_t srcY = (src / dimZ) % dimY;
        uint32_t srcZ = src % dimZ;

        for (uint32_t dst = 0; dst < numNodes; ++dst)
        {
            if (src == dst) continue;

            // Direct neighbors already have routing entries
            if (epSrc->GetRoutingDeviceIndex(static_cast<uint16_t>(dst)) != UINT32_MAX)
            {
                continue;
            }

            uint32_t dstX = dst / (dimY * dimZ);
            uint32_t dstY = (dst / dimZ) % dimY;
            uint32_t dstZ = dst % dimZ;

            // Dimension-order: route along X first, then Y, then Z
            uint32_t nextIdx;
            if (dstX != srcX)
            {
                uint32_t cwDist = (dstX > srcX) ? (dstX - srcX) : (dimX - srcX + dstX);
                uint32_t ccwDist = dimX - cwDist;
                uint32_t nextX = (cwDist <= ccwDist) ? ((srcX + 1) % dimX) : ((srcX + dimX - 1) % dimX);
                nextIdx = indexFromCoord(nextX, srcY, srcZ);
            }
            else if (dstY != srcY)
            {
                uint32_t cwDist = (dstY > srcY) ? (dstY - srcY) : (dimY - srcY + dstY);
                uint32_t ccwDist = dimY - cwDist;
                uint32_t nextY = (cwDist <= ccwDist) ? ((srcY + 1) % dimY) : ((srcY + dimY - 1) % dimY);
                nextIdx = indexFromCoord(srcX, nextY, srcZ);
            }
            else
            {
                uint32_t cwDist = (dstZ > srcZ) ? (dstZ - srcZ) : (dimZ - srcZ + dstZ);
                uint32_t ccwDist = dimZ - cwDist;
                uint32_t nextZ = (cwDist <= ccwDist) ? ((srcZ + 1) % dimZ) : ((srcZ + dimZ - 1) % dimZ);
                nextIdx = indexFromCoord(srcX, srcY, nextZ);
            }

            // Route to next-hop neighbor (which is a direct neighbor)
            uint32_t devIdx = epSrc->GetRoutingDeviceIndex(static_cast<uint16_t>(nextIdx));
            epSrc->SetRoutingEntry(static_cast<uint16_t>(dst), devIdx);
        }

        // Attach degradation model if set
        if (m_linkDegradationModel)
        {
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            epSrc->SetLinkDegradationModel(portModel);
        }
    }


    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::BuildMesh2D(uint32_t rows, uint32_t cols)
{
    NS_LOG_FUNCTION(this << rows << cols);

    uint32_t numNodes = rows * cols;
    NS_ABORT_MSG_IF(numNodes < 2, "Mesh topology requires at least 2 nodes");

    m_numGpus = numNodes;
    m_gpuNodes.Create(numNodes);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);
    m_switchNodes = NodeContainer();

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // Assign ranks
    for (uint32_t idx = 0; idx < numNodes; ++idx)
    {
        endpointHelper.SetRank(m_endpoints.Get(idx), idx);
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx));
        auto it = m_deviceTypes.find(static_cast<uint16_t>(idx));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
    }

    auto indexFromCoord = [&](uint32_t row, uint32_t col) { return row * cols + col; };

    // Helper lambda to install a link
    auto installLink = [&](uint32_t idx1, uint32_t idx2) {
        NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(idx1), m_gpuNodes.Get(idx2));
        Ptr<FabricEndpoint> ep1 = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx1));
        Ptr<FabricEndpoint> ep2 = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx2));

        uint32_t devIdx1 = ep1->GetNNetDevices();
        uint32_t devIdx2 = ep2->GetNNetDevices();

        ep1->AddNetDevice(devices.Get(0));
        ep2->AddNetDevice(devices.Get(1));

        Mac48Address addr1 = Mac48Address::ConvertFrom(devices.Get(0)->GetAddress());
        Mac48Address addr2 = Mac48Address::ConvertFrom(devices.Get(1)->GetAddress());

        ep1->SetRoutingEntry(static_cast<uint16_t>(idx2), devIdx1);
        ep1->SetNeighborMac(static_cast<uint16_t>(idx2), addr2);
        ep2->SetRoutingEntry(static_cast<uint16_t>(idx1), devIdx2);
        ep2->SetNeighborMac(static_cast<uint16_t>(idx1), addr1);
    };

    // Horizontal links (no wrap-around)
    for (uint32_t row = 0; row < rows; ++row)
    {
        for (uint32_t col = 0; col < cols - 1; ++col)
        {
            installLink(indexFromCoord(row, col), indexFromCoord(row, col + 1));
        }
    }

    // Vertical links (no wrap-around)
    for (uint32_t col = 0; col < cols; ++col)
    {
        for (uint32_t row = 0; row < rows - 1; ++row)
        {
            installLink(indexFromCoord(row, col), indexFromCoord(row + 1, col));
        }
    }

    // Populate dimension-order routing (X first, then Y)
    for (uint32_t src = 0; src < numNodes; ++src)
    {
        Ptr<FabricEndpoint> epSrc = DynamicCast<FabricEndpoint>(m_endpoints.Get(src));
        uint32_t srcRow = src / cols;
        uint32_t srcCol = src % cols;

        for (uint32_t dst = 0; dst < numNodes; ++dst)
        {
            if (src == dst) continue;

            if (epSrc->GetRoutingDeviceIndex(static_cast<uint16_t>(dst)) != UINT32_MAX)
            {
                continue;
            }

            uint32_t dstRow = dst / cols;
            uint32_t dstCol = dst % cols;

            // Dimension-order: route X (column) first, then Y (row)
            uint32_t nextIdx;
            if (dstCol != srcCol)
            {
                nextIdx = indexFromCoord(srcRow, srcCol + (dstCol > srcCol ? 1 : -1));
            }
            else
            {
                nextIdx = indexFromCoord(srcRow + (dstRow > srcRow ? 1 : -1), srcCol);
            }

            uint32_t devIdx = epSrc->GetRoutingDeviceIndex(static_cast<uint16_t>(nextIdx));
            epSrc->SetRoutingEntry(static_cast<uint16_t>(dst), devIdx);
        }

        if (m_linkDegradationModel)
        {
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            epSrc->SetLinkDegradationModel(portModel);
        }
    }


    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::BuildHypercube(uint32_t dimensions)
{
    NS_LOG_FUNCTION(this << dimensions);

    uint32_t numNodes = 1 << dimensions;
    NS_ABORT_MSG_IF(numNodes < 2, "Hypercube topology requires at least 2 nodes");

    m_numGpus = numNodes;
    m_gpuNodes.Create(numNodes);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);
    m_switchNodes = NodeContainer();

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // Assign ranks
    for (uint32_t idx = 0; idx < numNodes; ++idx)
    {
        endpointHelper.SetRank(m_endpoints.Get(idx), idx);
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx));
        auto it = m_deviceTypes.find(static_cast<uint16_t>(idx));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
    }

    // Helper lambda
    auto installLink = [&](uint32_t idx1, uint32_t idx2) {
        NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(idx1), m_gpuNodes.Get(idx2));
        Ptr<FabricEndpoint> ep1 = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx1));
        Ptr<FabricEndpoint> ep2 = DynamicCast<FabricEndpoint>(m_endpoints.Get(idx2));

        uint32_t devIdx1 = ep1->GetNNetDevices();
        uint32_t devIdx2 = ep2->GetNNetDevices();

        ep1->AddNetDevice(devices.Get(0));
        ep2->AddNetDevice(devices.Get(1));

        Mac48Address addr1 = Mac48Address::ConvertFrom(devices.Get(0)->GetAddress());
        Mac48Address addr2 = Mac48Address::ConvertFrom(devices.Get(1)->GetAddress());

        ep1->SetRoutingEntry(static_cast<uint16_t>(idx2), devIdx1);
        ep1->SetNeighborMac(static_cast<uint16_t>(idx2), addr2);
        ep2->SetRoutingEntry(static_cast<uint16_t>(idx1), devIdx2);
        ep2->SetNeighborMac(static_cast<uint16_t>(idx1), addr1);
    };

    // Connect each node to its hypercube neighbors
    for (uint32_t node = 0; node < numNodes; ++node)
    {
        for (uint32_t d = 0; d < dimensions; ++d)
        {
            uint32_t neighbor = node ^ (1 << d);
            if (node < neighbor)
            {
                installLink(node, neighbor);
            }
        }
    }

    // Populate bit-correction routing
    for (uint32_t src = 0; src < numNodes; ++src)
    {
        Ptr<FabricEndpoint> epSrc = DynamicCast<FabricEndpoint>(m_endpoints.Get(src));

        for (uint32_t dst = 0; dst < numNodes; ++dst)
        {
            if (src == dst) continue;

            if (epSrc->GetRoutingDeviceIndex(static_cast<uint16_t>(dst)) != UINT32_MAX)
            {
                continue;
            }

            // Bit-correction: route along lowest differing dimension
            uint32_t diff = src ^ dst;
            uint32_t lowestDim = 0;
            while (!(diff & (1 << lowestDim)))
            {
                lowestDim++;
            }

            uint32_t nextHop = src ^ (1 << lowestDim);
            uint32_t devIdx = epSrc->GetRoutingDeviceIndex(static_cast<uint16_t>(nextHop));
            epSrc->SetRoutingEntry(static_cast<uint16_t>(dst), devIdx);
        }

        if (m_linkDegradationModel)
        {
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            epSrc->SetLinkDegradationModel(portModel);
        }
    }


    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

void
GpuClusterTopologyHelper::SetDeviceType(uint16_t rank, DeviceType type)
{
    m_deviceTypes[rank] = type;
}

void
GpuClusterTopologyHelper::SetLinkDegradationModel(Ptr<LinkDegradationModel> model)
{
    m_linkDegradationModel = model;
}

void
GpuClusterTopologyHelper::SetBerTiers(double berIntraNodeElectrical,
                                       double berIntraRackElectrical,
                                       double berInterRackElectrical,
                                       double berInterRackOptical)
{
    m_berIntraNodeElectrical = berIntraNodeElectrical;
    m_berIntraRackElectrical = berIntraRackElectrical;
    m_berInterRackElectrical = berInterRackElectrical;
    m_berInterRackOptical = berInterRackOptical;
}

void
GpuClusterTopologyHelper::SetInterSwitchMedium(const std::string& medium)
{
    NS_ABORT_MSG_IF(medium != "electrical" && medium != "optical",
                    "Inter-switch medium must be electrical or optical");
    m_interSwitchMedium = medium;
}

Ptr<LinkDegradationModel>
GpuClusterTopologyHelper::MakePortModel(
    const std::string& linkClass,
    const std::string& medium,
    const std::string& protocol,
    double bandwidthGbps,
    double distanceMeters) const
{
    Ptr<LinkDegradationModel> portModel = CreateObject<LinkDegradationModel>();

    // Pick tier-appropriate BER. Zero (unset) falls back to the shared model's BER.
    double ber = 0.0;
    if (linkClass == "intra_node" && medium == "electrical")
    {
        ber = m_berIntraNodeElectrical;
    }
    else if (linkClass == "intra_rack" && medium == "electrical")
    {
        ber = m_berIntraRackElectrical;
    }
    else if ((linkClass == "inter_rack" || linkClass == "inter_row")
             && medium == "electrical")
    {
        ber = m_berInterRackElectrical;
    }
    else if ((linkClass == "inter_rack" || linkClass == "inter_row")
             && medium == "optical")
    {
        ber = m_berInterRackOptical;
    }
    if (ber <= 0.0 && m_linkDegradationModel)
    {
        ber = m_linkDegradationModel->GetBer();
    }
    // Explicitly apply zero as well; LinkDegradationModel's constructor has a
    // non-zero default that must not leak into an error-free tier.
    portModel->SetBer(ber);

    // Tag with full metadata for tracing / FEC selection.
    LinkMetadata meta;
    meta.linkClass = linkClass;
    meta.medium = medium;
    meta.distanceMeters = distanceMeters;
    meta.protocol = protocol;
    meta.bandwidthGbps = bandwidthGbps;
    meta.ber = ber;
    meta.fecProfile = "none";
    portModel->SetLinkMetadata(meta);

    // Propagate shared model's error mode / burst config if present.
    if (m_linkDegradationModel)
    {
        portModel->SetPacketLossRate(m_linkDegradationModel->GetPacketLossRate());
        portModel->SetErrorMode(m_linkDegradationModel->GetErrorMode());
        portModel->SetCodewordSize(m_linkDegradationModel->GetCodewordSize());
        portModel->SetBurstLength(m_linkDegradationModel->GetBurstLength());
        portModel->SetBurstArrivalRate(m_linkDegradationModel->GetBurstArrivalRate());
        Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();
        rng->SetStream(-1);
        portModel->SetRandomStream(rng);
    }
    return portModel;
}

void
GpuClusterTopologyHelper::SetFecModel(Ptr<FecModel> model)
{
    m_fecModel = model;
}

void
GpuClusterTopologyHelper::SetFecOpticalOnly(bool opticalOnly)
{
    m_fecOpticalOnly = opticalOnly;
}

void
GpuClusterTopologyHelper::SetSwitchLlrEnabled(bool enabled)
{
    m_switchLlrEnabled = enabled;
}

void
GpuClusterTopologyHelper::SetContentionModel(Ptr<ContentionModel> model)
{
    m_contentionModel = model;
}

Ptr<ContentionModel>
GpuClusterTopologyHelper::CreateContentionModelClone() const
{
    NS_LOG_FUNCTION(this);

    if (!m_contentionModel)
    {
        return nullptr;
    }

    Ptr<ContentionModel> clone = CreateObject<ContentionModel>();
    clone->SetCollectiveWeight(m_contentionModel->GetCollectiveWeight());
    clone->SetMemoryWeight(m_contentionModel->GetMemoryWeight());
    clone->SetP2pWeight(m_contentionModel->GetP2pWeight());

    // Parse m_dataRate (e.g., "100Gbps") to get bytes/sec for the contention model
    DataRate dr(m_dataRate);
    clone->SetBandwidth(dr.GetBitRate() / 8);  // bits/sec -> bytes/sec

    return clone;
}

void
GpuClusterTopologyHelper::AttachContentionModelToEndpoints()
{
    NS_LOG_FUNCTION(this);

    if (!m_contentionModel)
    {
        return;
    }

    Ptr<ContentionModel> contentionModel = CreateContentionModelClone();
    uint32_t numEndpoints = m_endpoints.GetN();
    for (uint32_t i = 0; i < numEndpoints; ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
        ep->SetContentionModel(contentionModel);
    }
}

void
GpuClusterTopologyHelper::SetFabricId(uint16_t fabricId)
{
    m_fabricId = fabricId;
}

uint16_t
GpuClusterTopologyHelper::GetFabricId() const
{
    return m_fabricId;
}

void
GpuClusterTopologyHelper::SetFabricType(FabricType type)
{
    m_fabricType = static_cast<uint8_t>(type);
}

FabricType
GpuClusterTopologyHelper::GetFabricType() const
{
    return static_cast<FabricType>(m_fabricType);
}

void
GpuClusterTopologyHelper::SetLinksPerGpu(uint32_t links)
{
    m_linksPerGpu = links;
}

uint32_t
GpuClusterTopologyHelper::GetLinksPerGpu() const
{
    return m_linksPerGpu;
}

void
GpuClusterTopologyHelper::SetSprayChunkSize(uint32_t bytes)
{
    m_sprayChunkSize = bytes;
}

uint32_t
GpuClusterTopologyHelper::GetSprayChunkSize() const
{
    return m_sprayChunkSize;
}

void
GpuClusterTopologyHelper::SetNumLanes(uint32_t numLanes)
{
    NS_ABORT_MSG_IF(numLanes < 1 || numLanes > 64, "numLanes must be between 1 and 64");
    m_numLanes = numLanes;
}

uint32_t
GpuClusterTopologyHelper::GetNumLanes() const
{
    return m_numLanes;
}

NodeContainer
GpuClusterTopologyHelper::GetGpuNodes() const
{
    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::GetSwitchNodes() const
{
    return m_switchNodes;
}

ApplicationContainer
GpuClusterTopologyHelper::GetEndpoints() const
{
    return m_endpoints;
}

ApplicationContainer
GpuClusterTopologyHelper::GetGpuEndpoints() const
{
    return m_endpoints;
}

NodeContainer
GpuClusterTopologyHelper::Build2DFullMesh(uint32_t rows, uint32_t cols)
{
    NS_LOG_FUNCTION(this << rows << cols);

    uint32_t numNodes = rows * cols;
    NS_ABORT_MSG_IF(numNodes < 2, "2D full-mesh requires at least 2 nodes");

    m_gpuNodes.Create(numNodes);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    for (uint32_t i = 0; i < numNodes; ++i)
    {
        endpointHelper.SetRank(m_endpoints.Get(i), i);
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        auto it = m_deviceTypes.find(static_cast<uint16_t>(i));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
    }

    m_switchNodes = NodeContainer();

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // 2D full-mesh: node(r,c) connects to all nodes in row r and column c
    for (uint32_t r = 0; r < rows; ++r)
    {
        for (uint32_t c = 0; c < cols; ++c)
        {
            uint32_t nodeId = r * cols + c;

            // Connect to all other nodes in same row
            for (uint32_t c2 = 0; c2 < cols; ++c2)
            {
                if (c2 == c) continue;
                uint32_t peerId = r * cols + c2;

                // Only create link once (from smaller ID to larger)
                if (peerId < nodeId) continue;

                Ptr<FabricEndpoint> epA = DynamicCast<FabricEndpoint>(m_endpoints.Get(nodeId));
                Ptr<FabricEndpoint> epB = DynamicCast<FabricEndpoint>(m_endpoints.Get(peerId));

                NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(nodeId), m_gpuNodes.Get(peerId));

                Ptr<PointToPointNetDevice> devA = DynamicCast<PointToPointNetDevice>(devices.Get(0));
                Ptr<PointToPointNetDevice> devB = DynamicCast<PointToPointNetDevice>(devices.Get(1));

                epA->AddNetDevice(devices.Get(0));
                epB->AddNetDevice(devices.Get(1));

                uint32_t devIdxA = epA->GetNNetDevices() - 1;
                uint32_t devIdxB = epB->GetNNetDevices() - 1;

                Mac48Address addrB = Mac48Address::ConvertFrom(devB->GetAddress());
                Mac48Address addrA = Mac48Address::ConvertFrom(devA->GetAddress());

                epA->SetRoutingEntry(static_cast<uint16_t>(peerId), devIdxA);
                epA->SetNeighborMac(static_cast<uint16_t>(peerId), addrB);

                epB->SetRoutingEntry(static_cast<uint16_t>(nodeId), devIdxB);
                epB->SetNeighborMac(static_cast<uint16_t>(nodeId), addrA);
            }

            // Connect to all other nodes in same column
            for (uint32_t r2 = 0; r2 < rows; ++r2)
            {
                if (r2 == r) continue;
                uint32_t peerId = r2 * cols + c;

                // Only create link once
                if (peerId < nodeId) continue;

                Ptr<FabricEndpoint> epA = DynamicCast<FabricEndpoint>(m_endpoints.Get(nodeId));
                Ptr<FabricEndpoint> epB = DynamicCast<FabricEndpoint>(m_endpoints.Get(peerId));

                NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(nodeId), m_gpuNodes.Get(peerId));

                Ptr<PointToPointNetDevice> devA = DynamicCast<PointToPointNetDevice>(devices.Get(0));
                Ptr<PointToPointNetDevice> devB = DynamicCast<PointToPointNetDevice>(devices.Get(1));

                epA->AddNetDevice(devices.Get(0));
                epB->AddNetDevice(devices.Get(1));

                uint32_t devIdxA = epA->GetNNetDevices() - 1;
                uint32_t devIdxB = epB->GetNNetDevices() - 1;

                Mac48Address addrB = Mac48Address::ConvertFrom(devB->GetAddress());
                Mac48Address addrA = Mac48Address::ConvertFrom(devA->GetAddress());

                epA->SetRoutingEntry(static_cast<uint16_t>(peerId), devIdxA);
                epA->SetNeighborMac(static_cast<uint16_t>(peerId), addrB);

                epB->SetRoutingEntry(static_cast<uint16_t>(nodeId), devIdxB);
                epB->SetNeighborMac(static_cast<uint16_t>(nodeId), addrA);
            }
        }
    }

    // Populate routes for destinations outside the source row and column.
    // A packet first crosses its source row to the destination column, then
    // crosses that column to the destination.
    for (uint32_t src = 0; src < numNodes; ++src)
    {
        Ptr<FabricEndpoint> epSrc = DynamicCast<FabricEndpoint>(m_endpoints.Get(src));
        uint32_t srcRow = src / cols;

        for (uint32_t dst = 0; dst < numNodes; ++dst)
        {
            if (src == dst ||
                epSrc->GetRoutingDeviceIndex(static_cast<uint16_t>(dst)) != UINT32_MAX)
            {
                continue;
            }

            uint32_t dstCol = dst % cols;
            uint32_t nextHop = srcRow * cols + dstCol;
            uint32_t devIdx =
                epSrc->GetRoutingDeviceIndex(static_cast<uint16_t>(nextHop));
            NS_ABORT_MSG_IF(devIdx == UINT32_MAX,
                            "2D full-mesh route has no direct next hop");
            epSrc->SetRoutingEntry(static_cast<uint16_t>(dst), devIdx);
        }
    }

    // Attach degradation model
    if (m_linkDegradationModel)
    {
        for (uint32_t i = 0; i < numNodes; ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            ep->SetLinkDegradationModel(portModel);
        }
    }


    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::BuildNDFullMesh(const std::vector<uint32_t>& dims)
{
    NS_LOG_FUNCTION(this);

    uint32_t numNodes = 1;
    for (uint32_t d : dims) numNodes *= d;
    NS_ABORT_MSG_IF(numNodes < 2, "nD full-mesh requires at least 2 nodes");

    m_gpuNodes.Create(numNodes);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    for (uint32_t i = 0; i < numNodes; ++i)
    {
        endpointHelper.SetRank(m_endpoints.Get(i), i);
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        auto it = m_deviceTypes.find(static_cast<uint16_t>(i));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
    }

    m_switchNodes = NodeContainer();

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    // nD full-mesh: within each 1D segment, all nodes have direct pairwise links
    // For dimension d, all nodes sharing same values in other dimensions form a 1D segment
    // Iterate over all pairs of nodes that differ in exactly 1 dimension
    for (uint32_t i = 0; i < numNodes; ++i)
    {
        // Compute coordinates of node i
        std::vector<uint32_t> coords(dims.size());
        uint32_t remaining = i;
        for (uint32_t d = dims.size() - 1; d >= 0 && d < dims.size(); --d)
        {
            coords[d] = remaining % dims[d];
            remaining /= dims[d];
        }

        // For each dimension, connect to all nodes in the same 1D segment
        for (uint32_t d = 0; d < dims.size(); ++d)
        {
            for (uint32_t v = 0; v < dims[d]; ++v)
            {
                if (v == coords[d]) continue;

                // Compute peer ID by changing coordinate in dimension d
                std::vector<uint32_t> peerCoords = coords;
                peerCoords[d] = v;
                uint32_t peerId = 0;
                for (uint32_t dd = 0; dd < dims.size(); ++dd)
                {
                    peerId = peerId * dims[dd] + peerCoords[dd];
                }

                // Only create link once (from smaller ID)
                if (peerId < i) continue;

                Ptr<FabricEndpoint> epA = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
                Ptr<FabricEndpoint> epB = DynamicCast<FabricEndpoint>(m_endpoints.Get(peerId));

                NetDeviceContainer devices = p2p.Install(m_gpuNodes.Get(i), m_gpuNodes.Get(peerId));

                Ptr<PointToPointNetDevice> devA = DynamicCast<PointToPointNetDevice>(devices.Get(0));
                Ptr<PointToPointNetDevice> devB = DynamicCast<PointToPointNetDevice>(devices.Get(1));

                epA->AddNetDevice(devices.Get(0));
                epB->AddNetDevice(devices.Get(1));

                uint32_t devIdxA = epA->GetNNetDevices() - 1;
                uint32_t devIdxB = epB->GetNNetDevices() - 1;

                Mac48Address addrB = Mac48Address::ConvertFrom(devB->GetAddress());
                Mac48Address addrA = Mac48Address::ConvertFrom(devA->GetAddress());

                epA->SetRoutingEntry(static_cast<uint16_t>(peerId), devIdxA);
                epA->SetNeighborMac(static_cast<uint16_t>(peerId), addrB);

                epB->SetRoutingEntry(static_cast<uint16_t>(i), devIdxB);
                epB->SetNeighborMac(static_cast<uint16_t>(i), addrA);
            }
        }
    }

    if (m_linkDegradationModel)
    {
        for (uint32_t i = 0; i < numNodes; ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            ep->SetLinkDegradationModel(portModel);
        }
    }


    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::Build2DFullMeshClos(uint32_t rackRows, uint32_t rackCols,
                                                uint32_t numRacks, uint32_t numSpineSwitches)
{
    NS_LOG_FUNCTION(this << rackRows << rackCols << numRacks << numSpineSwitches);

    uint32_t npusPerRack = rackRows * rackCols;
    uint32_t totalNpus = npusPerRack * numRacks;
    NS_ABORT_MSG_IF(totalNpus < 2, "2D fullmesh+Clos requires at least 2 NPUs");

    // Create NPU nodes across all racks
    m_gpuNodes.Create(totalNpus);

    // Create spine switch nodes (only needed for multi-rack)
    if (numRacks > 1 && numSpineSwitches > 0)
    {
        m_switchNodes.Create(numSpineSwitches);
    }

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    for (uint32_t i = 0; i < totalNpus; ++i)
    {
        endpointHelper.SetRank(m_endpoints.Get(i), i);
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        auto it = m_deviceTypes.find(static_cast<uint16_t>(i));
        if (it != m_deviceTypes.end())
        {
            ep->SetDeviceType(it->second);
        }
    }

    PointToPointHelper p2pIntra;
    p2pIntra.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2pIntra.SetChannelAttribute("Delay", StringValue(m_delay));
    p2pIntra.SetQueue("ns3::DropTailQueue<Packet>");

    // Build intra-rack 2D full-mesh for each rack
    for (uint32_t rack = 0; rack < numRacks; ++rack)
    {
        uint32_t rackBase = rack * npusPerRack;

        for (uint32_t r = 0; r < rackRows; ++r)
        {
            for (uint32_t c = 0; c < rackCols; ++c)
            {
                uint32_t nodeId = rackBase + r * rackCols + c;

                // Row connections
                for (uint32_t c2 = 0; c2 < rackCols; ++c2)
                {
                    if (c2 == c) continue;
                    uint32_t peerId = rackBase + r * rackCols + c2;
                    if (peerId < nodeId) continue;

                    Ptr<FabricEndpoint> epA = DynamicCast<FabricEndpoint>(m_endpoints.Get(nodeId));
                    Ptr<FabricEndpoint> epB = DynamicCast<FabricEndpoint>(m_endpoints.Get(peerId));

                    NetDeviceContainer devices = p2pIntra.Install(m_gpuNodes.Get(nodeId), m_gpuNodes.Get(peerId));
                    Ptr<PointToPointNetDevice> devA = DynamicCast<PointToPointNetDevice>(devices.Get(0));
                    Ptr<PointToPointNetDevice> devB = DynamicCast<PointToPointNetDevice>(devices.Get(1));

                    epA->AddNetDevice(devices.Get(0));
                    epB->AddNetDevice(devices.Get(1));

                    uint32_t devIdxA = epA->GetNNetDevices() - 1;
                    uint32_t devIdxB = epB->GetNNetDevices() - 1;

                    Mac48Address addrB = Mac48Address::ConvertFrom(devB->GetAddress());
                    Mac48Address addrA = Mac48Address::ConvertFrom(devA->GetAddress());

                    epA->SetRoutingEntry(static_cast<uint16_t>(peerId), devIdxA);
                    epA->SetNeighborMac(static_cast<uint16_t>(peerId), addrB);

                    epB->SetRoutingEntry(static_cast<uint16_t>(nodeId), devIdxB);
                    epB->SetNeighborMac(static_cast<uint16_t>(nodeId), addrA);
                }

                // Column connections
                for (uint32_t r2 = 0; r2 < rackRows; ++r2)
                {
                    if (r2 == r) continue;
                    uint32_t peerId = rackBase + r2 * rackCols + c;
                    if (peerId < nodeId) continue;

                    Ptr<FabricEndpoint> epA = DynamicCast<FabricEndpoint>(m_endpoints.Get(nodeId));
                    Ptr<FabricEndpoint> epB = DynamicCast<FabricEndpoint>(m_endpoints.Get(peerId));

                    NetDeviceContainer devices = p2pIntra.Install(m_gpuNodes.Get(nodeId), m_gpuNodes.Get(peerId));
                    Ptr<PointToPointNetDevice> devA = DynamicCast<PointToPointNetDevice>(devices.Get(0));
                    Ptr<PointToPointNetDevice> devB = DynamicCast<PointToPointNetDevice>(devices.Get(1));

                    epA->AddNetDevice(devices.Get(0));
                    epB->AddNetDevice(devices.Get(1));

                    uint32_t devIdxA = epA->GetNNetDevices() - 1;
                    uint32_t devIdxB = epB->GetNNetDevices() - 1;

                    Mac48Address addrB = Mac48Address::ConvertFrom(devB->GetAddress());
                    Mac48Address addrA = Mac48Address::ConvertFrom(devA->GetAddress());

                    epA->SetRoutingEntry(static_cast<uint16_t>(peerId), devIdxA);
                    epA->SetNeighborMac(static_cast<uint16_t>(peerId), addrB);

                    epB->SetRoutingEntry(static_cast<uint16_t>(nodeId), devIdxB);
                    epB->SetNeighborMac(static_cast<uint16_t>(nodeId), addrA);
                }
            }
        }
    }

    // Build inter-rack spine switch connections (if multi-rack)
    if (numRacks > 1 && numSpineSwitches > 0)
    {
        PointToPointHelper p2pInter;
        p2pInter.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
        p2pInter.SetChannelAttribute("Delay", StringValue("300ns")); // Higher latency for inter-rack
        p2pInter.SetQueue("ns3::DropTailQueue<Packet>");

        NvSwitchHelper switchHelper;

        // Install spine switches
        std::vector<Ptr<NetDevice>> spineSwitches;
        for (uint32_t s = 0; s < numSpineSwitches; ++s)
        {
            Ptr<NetDevice> swDev = switchHelper.Install(m_switchNodes.Get(s));
            spineSwitches.push_back(swDev);
        }

        // Connect border nodes (first node per row/col) from each rack to spine switches
        // Each rack's NPU 0, rackRows-1*cols, etc. connect to spine switches
        for (uint32_t rack = 0; rack < numRacks; ++rack)
        {
            uint32_t rackBase = rack * npusPerRack;

            // Connect every NPU in first row and first column to spine switches
            // This gives each rack multiple uplinks to spine switches
            std::vector<uint32_t> borderNodes;

            // First row (column 0..cols-1)
            for (uint32_t c = 0; c < rackCols; ++c)
            {
                borderNodes.push_back(rackBase + c);
            }
            // First column (row 0 already covered, add row 1..rows-1)
            for (uint32_t r = 1; r < rackRows; ++r)
            {
                borderNodes.push_back(rackBase + r * rackCols);
            }

            // Connect border nodes to spine switches
            for (uint32_t bn : borderNodes)
            {
                for (uint32_t s = 0; s < numSpineSwitches; ++s)
                {
                    NetDeviceContainer devices = p2pInter.Install(m_gpuNodes.Get(bn), m_switchNodes.Get(s));

                    Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(bn));
                    ep->AddNetDevice(devices.Get(0));

                    uint32_t devIdx = ep->GetNNetDevices() - 1;
                    Mac48Address spineAddr = Mac48Address::ConvertFrom(
                        DynamicCast<PointToPointNetDevice>(devices.Get(1))->GetAddress());

                    switchHelper.AddPort(spineSwitches[s], devices.Get(1));

                    // For inter-rack routing, border nodes route to spine switch
                    // Spine switch will forward to the correct rack
                    // We set routing for remote rack NPUs through spine switch port
                    for (uint32_t remoteRack = 0; remoteRack < numRacks; ++remoteRack)
                    {
                        if (remoteRack == rack) continue;
                        uint32_t remoteBase = remoteRack * npusPerRack;

                        // For each remote NPU, route through spine switch
                        // Border nodes that are connected to spine switches handle inter-rack traffic
                        // Intra-rack routing is already handled by 2D fullmesh
                        // We only need inter-rack routing here
                        for (uint32_t rn = 0; rn < npusPerRack; ++rn)
                        {
                            uint16_t remoteNpuId = static_cast<uint16_t>(remoteBase + rn);
                            ep->SetRoutingEntry(remoteNpuId, devIdx);
                            ep->SetNeighborMac(remoteNpuId, spineAddr);
                        }
                    }
                }
            }
        }
    }

    // Attach degradation model
    if (m_linkDegradationModel)
    {
        for (uint32_t i = 0; i < totalNpus; ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            ep->SetLinkDegradationModel(portModel);
        }
    }


    AttachContentionModelToEndpoints();

    return m_gpuNodes;
}

NodeContainer
GpuClusterTopologyHelper::Build3LevelHierarchical(uint32_t numLeafSwitches,
                                                    uint32_t numMidSwitches,
                                                    uint32_t numSpineSwitches)
{
    NS_LOG_FUNCTION(this << numLeafSwitches << numMidSwitches << numSpineSwitches);
    NS_ABORT_MSG_IF(m_numGpus < 1, "3-level hierarchical requires at least 1 GPU");
    NS_ABORT_MSG_IF(numLeafSwitches < 1, "3-level hierarchical requires >= 1 leaf switch");
    NS_ABORT_MSG_IF(numMidSwitches < 1, "3-level hierarchical requires >= 1 mid switch");
    NS_ABORT_MSG_IF(numSpineSwitches < 1, "3-level hierarchical requires >= 1 spine switch");
    NS_ABORT_MSG_IF(m_numGpus < numLeafSwitches, "GPUs must be >= leaf switches");
    NS_ABORT_MSG_IF(m_numGpus % numLeafSwitches != 0, "GPUs must divide evenly by leaf switches");
    NS_ABORT_MSG_IF(m_linksPerGpu < 1, "3-level hierarchical requires at least one link per GPU");
    NS_ABORT_MSG_IF(numLeafSwitches % numMidSwitches != 0,
                    "3-level hierarchical requires leaf switches divisible by mid switches");

    uint32_t gpusPerLeaf = m_numGpus / numLeafSwitches;
    uint32_t leavesPerMid = numLeafSwitches / numMidSwitches;

    m_gpuNodes.Create(m_numGpus);
    m_switchNodes.Create(numLeafSwitches + numMidSwitches + numSpineSwitches);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    NvSwitchHelper switchHelper;
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    std::vector<Ptr<NetDevice>> leafSwitches, midSwitches, spineSwitches;
    for (uint32_t i = 0; i < numLeafSwitches; ++i)
    {
        leafSwitches.push_back(switchHelper.Install(m_switchNodes.Get(i)));
    }
    for (uint32_t i = 0; i < numMidSwitches; ++i)
    {
        midSwitches.push_back(switchHelper.Install(m_switchNodes.Get(numLeafSwitches + i)));
    }
    for (uint32_t i = 0; i < numSpineSwitches; ++i)
    {
        spineSwitches.push_back(switchHelper.Install(m_switchNodes.Get(numLeafSwitches + numMidSwitches + i)));
    }

    std::vector<Mac48Address> gpuMacs(m_numGpus);
    // Track GPU ports on each leaf switch for static and spray routing.
    std::vector<std::vector<uint32_t>> leafGpuPorts(numLeafSwitches,
                                                      std::vector<uint32_t>(m_numGpus, 0));
    std::vector<std::vector<std::vector<uint32_t>>> leafGpuAllPorts(
        numLeafSwitches, std::vector<std::vector<uint32_t>>(m_numGpus));

    // Each GPU attaches to its home leaf. Parallel links increase endpoint
    // injection bandwidth but do not bypass the hierarchy.
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        endpointHelper.SetRank(m_endpoints.Get(gpu), gpu);
        auto it = m_deviceTypes.find(static_cast<uint16_t>(gpu));
        if (it != m_deviceTypes.end()) { ep->SetDeviceType(it->second); }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        if (m_linksPerGpu > 1)
        {
            ep->SetSprayingEnabled(true);
            ep->SetSprayChunkSize(m_sprayChunkSize);
        }

        uint32_t leaf = gpu / gpusPerLeaf;
        for (uint32_t link = 0; link < m_linksPerGpu; ++link)
        {
            NetDeviceContainer devPair = p2p.Install(
                m_gpuNodes.Get(gpu), m_switchNodes.Get(leaf));
            ep->AddNetDevice(devPair.Get(0));
            uint32_t portNum = switchHelper.AddPort(leafSwitches[leaf], devPair.Get(1));
            leafGpuAllPorts[leaf][gpu].push_back(portNum);
            if (link == 0)
            {
                leafGpuPorts[leaf][gpu] = portNum;
                gpuMacs[gpu] = Mac48Address::ConvertFrom(devPair.Get(0)->GetAddress());
            }
            if (m_linkDegradationModel)
            {
                Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
                if (leafSw)
                {
                    leafSw->SetPortDegradationModel(
                        portNum,
                        MakePortModel("intra_rack", "electrical", "NVLink", 0.0, 2.0));
                }
            }
        }
        uint32_t leafGroup = gpu / gpusPerLeaf;
        ep->SetNodeId(leafGroup);
        ep->SetLocalRankRange(static_cast<uint16_t>(leafGroup * gpusPerLeaf),
                              static_cast<uint16_t>(gpusPerLeaf));
    }

    // Leaf-to-mid links remain electrical. Each leaf connects to its home mid;
    // communication between mid groups uses the spine tier.
    std::vector<std::vector<uint32_t>> leafMidPorts(numLeafSwitches,
                                                      std::vector<uint32_t>(numMidSwitches, 0));
    std::vector<std::vector<uint32_t>> midLeafPorts(numMidSwitches,
                                                      std::vector<uint32_t>(numLeafSwitches, 0));
    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        const uint32_t mid = leaf / leavesPerMid;
        NetDeviceContainer devPair = p2p.Install(
            m_switchNodes.Get(leaf),
            m_switchNodes.Get(numLeafSwitches + mid));
        leafMidPorts[leaf][mid] = switchHelper.AddPort(leafSwitches[leaf], devPair.Get(0));
        midLeafPorts[mid][leaf] = switchHelper.AddPort(midSwitches[mid], devPair.Get(1));
        if (m_linkDegradationModel)
        {
            Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
            Ptr<NvSwitch> midSw = DynamicCast<NvSwitch>(midSwitches[mid]);
            if (leafSw)
            {
                leafSw->SetPortDegradationModel(
                    leafMidPorts[leaf][mid],
                    MakePortModel("inter_rack", "electrical", "ScaleUp", 0.0, 3.0));
            }
            if (midSw)
            {
                midSw->SetPortDegradationModel(
                    midLeafPorts[mid][leaf],
                    MakePortModel("inter_rack", "electrical", "ScaleUp", 0.0, 3.0));
            }
        }
    }

    // Mid-to-spine links use the configured medium and carry traffic between
    // different mid-switch groups.
    std::vector<std::vector<uint32_t>> midSpinePorts(numMidSwitches,
                                                       std::vector<uint32_t>(numSpineSwitches, 0));
    std::vector<std::vector<uint32_t>> spineMidPorts(numSpineSwitches,
                                                       std::vector<uint32_t>(numMidSwitches, 0));
    for (uint32_t mid = 0; mid < numMidSwitches; ++mid)
    {
        for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
        {
            NetDeviceContainer devPair = p2p.Install(
                m_switchNodes.Get(numLeafSwitches + mid),
                m_switchNodes.Get(numLeafSwitches + numMidSwitches + spine));
            midSpinePorts[mid][spine] = switchHelper.AddPort(midSwitches[mid], devPair.Get(0));
            spineMidPorts[spine][mid] = switchHelper.AddPort(spineSwitches[spine], devPair.Get(1));
            if (m_linkDegradationModel)
            {
                const double distanceMeters =
                    (m_interSwitchMedium == "optical") ? 10.0 : 3.0;
                Ptr<NvSwitch> midSw = DynamicCast<NvSwitch>(midSwitches[mid]);
                Ptr<NvSwitch> spineSw = DynamicCast<NvSwitch>(spineSwitches[spine]);
                if (midSw)
                {
                    midSw->SetPortDegradationModel(
                        midSpinePorts[mid][spine],
                        MakePortModel("inter_rack",
                                      m_interSwitchMedium,
                                      "ScaleUp",
                                      0.0,
                                      distanceMeters));
                }
                if (spineSw)
                {
                    spineSw->SetPortDegradationModel(
                        spineMidPorts[spine][mid],
                        MakePortModel("inter_rack",
                                      m_interSwitchMedium,
                                      "ScaleUp",
                                      0.0,
                                      distanceMeters));
                }
            }
        }
    }

    // A leaf sends local destinations to their GPU ports and all remote
    // destinations to the leaf's home mid switch.
    for (uint32_t leaf = 0; leaf < numLeafSwitches; ++leaf)
    {
        Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
        if (!leafSw) continue;
        uint32_t homeMid = leaf / leavesPerMid;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t gpuLeaf = gpu / gpusPerLeaf;
            if (gpuLeaf == leaf)
            {
                leafSw->AddStaticRoute(gpuMacs[gpu], leafGpuPorts[leaf][gpu]);
                if (m_linksPerGpu > 1)
                {
                    leafSw->AddSprayPorts(gpuMacs[gpu], leafGpuAllPorts[leaf][gpu]);
                }
            }
            else
            {
                leafSw->AddStaticRoute(gpuMacs[gpu], leafMidPorts[leaf][homeMid]);
            }
        }
        leafSw->SetSprayRouting(true);
        if (m_fecModel) { leafSw->SetFecModel(m_fecModel); }
        leafSw->SetFecOpticalOnly(m_fecOpticalOnly);
        leafSw->SetLlrEnabled(m_switchLlrEnabled);
    }

    // A mid switch sends destinations in its group toward their leaf. Traffic
    // for another group traverses a spine selected from the destination rank.
    for (uint32_t mid = 0; mid < numMidSwitches; ++mid)
    {
        Ptr<NvSwitch> midSw = DynamicCast<NvSwitch>(midSwitches[mid]);
        if (!midSw) continue;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t gpuLeaf = gpu / gpusPerLeaf;
            uint32_t gpuMid = gpuLeaf / leavesPerMid;
            if (gpuMid == mid)
            {
                midSw->AddStaticRoute(gpuMacs[gpu], midLeafPorts[mid][gpuLeaf]);
            }
            else
            {
                midSw->AddStaticRoute(
                    gpuMacs[gpu], midSpinePorts[mid][gpu % numSpineSwitches]);
            }
        }
        midSw->SetSprayRouting(true);
        if (m_fecModel) { midSw->SetFecModel(m_fecModel); }
        midSw->SetFecOpticalOnly(m_fecOpticalOnly);
        midSw->SetLlrEnabled(m_switchLlrEnabled);
    }

    // A spine forwards to the destination's home mid switch.
    for (uint32_t spine = 0; spine < numSpineSwitches; ++spine)
    {
        Ptr<NvSwitch> spineSw = DynamicCast<NvSwitch>(spineSwitches[spine]);
        if (!spineSw) continue;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t gpuLeaf = gpu / gpusPerLeaf;
            uint32_t gpuMid = gpuLeaf / leavesPerMid;
            spineSw->AddStaticRoute(gpuMacs[gpu], spineMidPorts[spine][gpuMid]);
        }
        spineSw->SetSprayRouting(true);
        if (m_fecModel) { spineSw->SetFecModel(m_fecModel); }
        spineSw->SetFecOpticalOnly(m_fecOpticalOnly);
        spineSw->SetLlrEnabled(m_switchLlrEnabled);
    }

    // Every destination leaves through one of the source GPU's links to its
    // home leaf; switch routing selects the remaining path.
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        for (uint32_t dest = 0; dest < m_numGpus; ++dest)
        {
            if (dest == gpu) continue;
            std::vector<uint32_t> destDevices;
            for (uint32_t link = 0; link < m_linksPerGpu; ++link)
            {
                destDevices.push_back(link);
            }
            ep->SetRoutingDevices(static_cast<uint16_t>(dest), destDevices);
            ep->SetNeighborMac(static_cast<uint16_t>(dest), gpuMacs[dest]);
        }
    }

    // Attach degradation models to switch ports
    if (m_linkDegradationModel)
    {
        for (uint32_t i = 0; i < m_endpoints.GetN(); ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            ep->SetLinkDegradationModel(portModel);
        }
    }

    AttachContentionModelToEndpoints();
    return NodeContainer(m_gpuNodes, m_switchNodes);
}

NodeContainer
GpuClusterTopologyHelper::BuildDragonflyPlus(uint32_t numGroups, uint32_t routersPerGroup)
{
    NS_LOG_FUNCTION(this << numGroups << routersPerGroup);
    NS_ABORT_MSG_IF(m_numGpus < 1, "Dragonfly+ requires at least 1 GPU");
    NS_ABORT_MSG_IF(numGroups < 1, "Dragonfly+ requires >= 1 group");
    NS_ABORT_MSG_IF(routersPerGroup < 1, "Dragonfly+ requires >= 1 router per group");
    uint32_t totalLeafSwitches = numGroups * routersPerGroup;
    NS_ABORT_MSG_IF(m_numGpus < totalLeafSwitches, "GPUs must be >= groups*routers");
    NS_ABORT_MSG_IF(m_numGpus % totalLeafSwitches != 0, "GPUs must divide evenly by groups*routers");
    NS_ABORT_MSG_IF(m_linksPerGpu < 1, "Dragonfly+ requires at least one link per GPU");

    uint32_t gpusPerLeaf = m_numGpus / totalLeafSwitches;

    m_gpuNodes.Create(m_numGpus);
    m_switchNodes.Create(totalLeafSwitches);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    NvSwitchHelper switchHelper;
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    std::vector<Ptr<NetDevice>> leafSwitches;
    for (uint32_t i = 0; i < totalLeafSwitches; ++i)
    {
        leafSwitches.push_back(switchHelper.Install(m_switchNodes.Get(i)));
    }

    std::vector<Mac48Address> gpuMacs(m_numGpus);
    std::vector<std::vector<uint32_t>> leafGpuPorts(
        totalLeafSwitches, std::vector<uint32_t>(m_numGpus, 0));
    std::vector<std::vector<std::vector<uint32_t>>> leafGpuAllPorts(
        totalLeafSwitches, std::vector<std::vector<uint32_t>>(m_numGpus));
    std::vector<std::vector<uint32_t>> leafToLeafPorts(
        totalLeafSwitches, std::vector<uint32_t>(totalLeafSwitches, 0));

    // Each GPU attaches only to its home leaf. Parallel links increase local
    // injection bandwidth; they do not bypass the Dragonfly+ router path.
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        endpointHelper.SetRank(m_endpoints.Get(gpu), gpu);
        auto it = m_deviceTypes.find(static_cast<uint16_t>(gpu));
        if (it != m_deviceTypes.end()) { ep->SetDeviceType(it->second); }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        if (m_linksPerGpu > 1)
        {
            ep->SetSprayingEnabled(true);
            ep->SetSprayChunkSize(m_sprayChunkSize);
        }

        uint32_t leaf = gpu / gpusPerLeaf;
        for (uint32_t link = 0; link < m_linksPerGpu; ++link)
        {
            NetDeviceContainer devPair = p2p.Install(
                m_gpuNodes.Get(gpu), m_switchNodes.Get(leaf));
            ep->AddNetDevice(devPair.Get(0));
            uint32_t portNum = switchHelper.AddPort(leafSwitches[leaf], devPair.Get(1));
            leafGpuAllPorts[leaf][gpu].push_back(portNum);
            if (link == 0)
            {
                leafGpuPorts[leaf][gpu] = portNum;
                gpuMacs[gpu] = Mac48Address::ConvertFrom(devPair.Get(0)->GetAddress());
            }
            if (m_linkDegradationModel)
            {
                Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
                if (leafSw)
                {
                    leafSw->SetPortDegradationModel(
                        portNum,
                        MakePortModel("intra_rack", "electrical", "NVLink", 0.0, 2.0));
                }
            }
        }
        uint32_t group = gpu / (gpusPerLeaf * routersPerGroup);
        ep->SetNodeId(group);
        ep->SetLocalRankRange(static_cast<uint16_t>(group * gpusPerLeaf * routersPerGroup),
                              static_cast<uint16_t>(gpusPerLeaf * routersPerGroup));
    }

    // Intra-group full-mesh: connect leaf switches within each group pairwise
    for (uint32_t g = 0; g < numGroups; ++g)
    {
        for (uint32_t a = 0; a < routersPerGroup; ++a)
        {
            for (uint32_t b = a + 1; b < routersPerGroup; ++b)
            {
                uint32_t leafA = g * routersPerGroup + a;
                uint32_t leafB = g * routersPerGroup + b;
                NetDeviceContainer devPair = p2p.Install(
                    m_switchNodes.Get(leafA), m_switchNodes.Get(leafB));
                uint32_t portA = switchHelper.AddPort(leafSwitches[leafA], devPair.Get(0));
                uint32_t portB = switchHelper.AddPort(leafSwitches[leafB], devPair.Get(1));
                leafToLeafPorts[leafA][leafB] = portA;
                leafToLeafPorts[leafB][leafA] = portB;
                if (m_linkDegradationModel)
                {
                    Ptr<NvSwitch> switchA = DynamicCast<NvSwitch>(leafSwitches[leafA]);
                    Ptr<NvSwitch> switchB = DynamicCast<NvSwitch>(leafSwitches[leafB]);
                    if (switchA)
                    {
                        switchA->SetPortDegradationModel(
                            portA,
                            MakePortModel("intra_rack", "electrical", "ScaleUp", 0.0, 3.0));
                    }
                    if (switchB)
                    {
                        switchB->SetPortDegradationModel(
                            portB,
                            MakePortModel("intra_rack", "electrical", "ScaleUp", 0.0, 3.0));
                    }
                }
            }
        }
    }

    // Inter-group global links connect the gateway router (router 0) in every
    // group pair. These links use the configured inter-switch medium.
    for (uint32_t g1 = 0; g1 < numGroups; ++g1)
    {
        for (uint32_t g2 = g1 + 1; g2 < numGroups; ++g2)
        {
            uint32_t leaf1 = g1 * routersPerGroup;
            uint32_t leaf2 = g2 * routersPerGroup;
            NetDeviceContainer devPair = p2p.Install(
                m_switchNodes.Get(leaf1), m_switchNodes.Get(leaf2));
            uint32_t port1 = switchHelper.AddPort(leafSwitches[leaf1], devPair.Get(0));
            uint32_t port2 = switchHelper.AddPort(leafSwitches[leaf2], devPair.Get(1));
            leafToLeafPorts[leaf1][leaf2] = port1;
            leafToLeafPorts[leaf2][leaf1] = port2;
            if (m_linkDegradationModel)
            {
                const double distanceMeters =
                    (m_interSwitchMedium == "optical") ? 10.0 : 3.0;
                Ptr<NvSwitch> switch1 = DynamicCast<NvSwitch>(leafSwitches[leaf1]);
                Ptr<NvSwitch> switch2 = DynamicCast<NvSwitch>(leafSwitches[leaf2]);
                if (switch1)
                {
                    switch1->SetPortDegradationModel(
                        port1,
                        MakePortModel("inter_rack", m_interSwitchMedium,
                                      "ScaleUp", 0.0, distanceMeters));
                }
                if (switch2)
                {
                    switch2->SetPortDegradationModel(
                        port2,
                        MakePortModel("inter_rack", m_interSwitchMedium,
                                      "ScaleUp", 0.0, distanceMeters));
                }
            }
        }
    }

    // Route by destination GPU. Traffic first reaches the source group's
    // gateway, crosses one global link, and then reaches the destination leaf.
    for (uint32_t leaf = 0; leaf < totalLeafSwitches; ++leaf)
    {
        Ptr<NvSwitch> leafSw = DynamicCast<NvSwitch>(leafSwitches[leaf]);
        if (!leafSw) continue;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            uint32_t destLeaf = gpu / gpusPerLeaf;
            if (destLeaf == leaf)
            {
                leafSw->AddStaticRoute(gpuMacs[gpu], leafGpuPorts[leaf][gpu]);
                if (m_linksPerGpu > 1)
                {
                    leafSw->AddSprayPorts(gpuMacs[gpu], leafGpuAllPorts[leaf][gpu]);
                }
                continue;
            }

            uint32_t group = leaf / routersPerGroup;
            uint32_t destGroup = destLeaf / routersPerGroup;
            uint32_t nextLeaf;
            if (group == destGroup)
            {
                nextLeaf = destLeaf;
            }
            else
            {
                uint32_t gateway = group * routersPerGroup;
                uint32_t destGateway = destGroup * routersPerGroup;
                nextLeaf = (leaf == gateway) ? destGateway : gateway;
            }
            leafSw->AddStaticRoute(gpuMacs[gpu], leafToLeafPorts[leaf][nextLeaf]);
        }
        leafSw->SetSprayRouting(true);
        if (m_fecModel) { leafSw->SetFecModel(m_fecModel); }
        leafSw->SetFecOpticalOnly(m_fecOpticalOnly);
        leafSw->SetLlrEnabled(m_switchLlrEnabled);
    }

    // Every destination leaves through the source GPU's home leaf.
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        for (uint32_t dest = 0; dest < m_numGpus; ++dest)
        {
            if (dest == gpu) continue;
            std::vector<uint32_t> destDevices;
            for (uint32_t link = 0; link < m_linksPerGpu; ++link)
            {
                destDevices.push_back(link);
            }
            ep->SetRoutingDevices(static_cast<uint16_t>(dest), destDevices);
            ep->SetNeighborMac(static_cast<uint16_t>(dest), gpuMacs[dest]);
        }
    }

    if (m_linkDegradationModel)
    {
        for (uint32_t i = 0; i < m_endpoints.GetN(); ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_rack", "electrical", "NVLink", 0.0, 2.0);
            ep->SetLinkDegradationModel(portModel);
        }
    }

    AttachContentionModelToEndpoints();
    return NodeContainer(m_gpuNodes, m_switchNodes);
}

NodeContainer
GpuClusterTopologyHelper::BuildMultiPlane(uint32_t numPlanes)
{
    NS_LOG_FUNCTION(this << numPlanes);
    NS_ABORT_MSG_IF(m_numGpus < 1, "Multi-plane requires at least 1 GPU");
    NS_ABORT_MSG_IF(numPlanes < 1, "Multi-plane requires >= 1 plane");
    NS_ABORT_MSG_IF(m_linksPerGpu < numPlanes, "Links per GPU must be >= numPlanes");

    uint32_t linksPerGpuPerPlane = m_linksPerGpu / numPlanes;

    m_gpuNodes.Create(m_numGpus);
    m_switchNodes.Create(numPlanes);

    FabricEndpointHelper endpointHelper;
    m_endpoints = endpointHelper.Install(m_gpuNodes);

    NvSwitchHelper switchHelper;
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(m_dataRate));
    p2p.SetChannelAttribute("Delay", StringValue(m_delay));
    p2p.SetQueue("ns3::DropTailQueue<Packet>");

    std::vector<Ptr<NetDevice>> planes;
    for (uint32_t p = 0; p < numPlanes; ++p)
    {
        planes.push_back(switchHelper.Install(m_switchNodes.Get(p)));
    }

    std::vector<Mac48Address> gpuMacs(m_numGpus);
    std::vector<std::vector<uint32_t>> planeGpuPorts(numPlanes,
                                                       std::vector<uint32_t>(m_numGpus, 0));

    // GPU -> plane links (intra_node electrical)
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        endpointHelper.SetRank(m_endpoints.Get(gpu), gpu);
        auto it = m_deviceTypes.find(static_cast<uint16_t>(gpu));
        if (it != m_deviceTypes.end()) { ep->SetDeviceType(it->second); }
        ep->SetFabricType(static_cast<FabricType>(m_fabricType));
        if (numPlanes > 1)
        {
            ep->SetSprayingEnabled(true);
            ep->SetSprayChunkSize(m_sprayChunkSize);
        }

        for (uint32_t plane = 0; plane < numPlanes; ++plane)
        {
            for (uint32_t link = 0; link < linksPerGpuPerPlane; ++link)
            {
                NetDeviceContainer devPair = p2p.Install(m_gpuNodes.Get(gpu), m_switchNodes.Get(plane));
                ep->AddNetDevice(devPair.Get(0));
                uint32_t portNum = switchHelper.AddPort(planes[plane], devPair.Get(1));
                if (link == 0) { planeGpuPorts[plane][gpu] = portNum; }
                if (link == 0 && plane == 0) { gpuMacs[gpu] = Mac48Address::ConvertFrom(devPair.Get(0)->GetAddress()); }
            }
        }
        // Single node, all GPUs local to plane 0's group
        ep->SetNodeId(0);
        ep->SetLocalRankRange(0, static_cast<uint16_t>(m_numGpus));
    }

    // Switch routing: each plane knows all GPUs via their direct port.
    for (uint32_t p = 0; p < numPlanes; ++p)
    {
        Ptr<NvSwitch> sw = DynamicCast<NvSwitch>(planes[p]);
        if (!sw) continue;
        for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
        {
            sw->AddStaticRoute(gpuMacs[gpu], planeGpuPorts[p][gpu]);
        }
        sw->SetSprayRouting(true);
        if (m_fecModel) { sw->SetFecModel(m_fecModel); }
        sw->SetLlrEnabled(m_switchLlrEnabled);
    }

    // Routing: dest -> all planes (spraying across planes)
    for (uint32_t gpu = 0; gpu < m_numGpus; ++gpu)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(gpu));
        for (uint32_t dest = 0; dest < m_numGpus; ++dest)
        {
            if (dest == gpu) continue;
            uint32_t destDeviceBase = 0;  // device index = plane * linksPerGpuPerPlane
            (void)destDeviceBase;
            std::vector<uint32_t> destDevices;
            for (uint32_t plane = 0; plane < numPlanes; ++plane)
            {
                for (uint32_t link = 0; link < linksPerGpuPerPlane; ++link)
                {
                    destDevices.push_back(plane * linksPerGpuPerPlane + link);
                }
            }
            ep->SetRoutingDevices(static_cast<uint16_t>(dest), destDevices);
            ep->SetNeighborMac(static_cast<uint16_t>(dest), gpuMacs[dest]);
        }
    }

    if (m_linkDegradationModel)
    {
        for (uint32_t i = 0; i < m_endpoints.GetN(); ++i)
        {
            Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
            Ptr<LinkDegradationModel> portModel = MakePortModel(
                "intra_node", "electrical", "NVLink", 0.0, 0.5);
            ep->SetLinkDegradationModel(portModel);
        }
    }

    AttachContentionModelToEndpoints();
    return m_gpuNodes;
}

// Recursive balanced-BST build over a locality-sorted rank permutation.
// For range [lo,hi) in sortedOrder, the median element is the subtree root;
// returns the root rank and fills parent/children (in rank space). The caller
// wires the returned child roots into the parent's left/right pointers.
static uint16_t
BuildLocalityTree(const std::vector<uint16_t>& sortedOrder,
                  uint32_t lo, uint32_t hi, uint16_t parent,
                  std::vector<uint16_t>& parentOut,
                  std::vector<uint16_t>& leftOut,
                  std::vector<uint16_t>& rightOut)
{
    if (lo >= hi)
    {
        return 0xFFFF;
    }
    uint32_t mid = lo + (hi - lo) / 2;
    uint16_t root = sortedOrder[mid];
    parentOut[root] = parent;            // 0xFFFF for the tree root
    uint16_t lc = BuildLocalityTree(sortedOrder, lo, mid, root,
                                    parentOut, leftOut, rightOut);
    uint16_t rc = BuildLocalityTree(sortedOrder, mid + 1, hi, root,
                                    parentOut, leftOut, rightOut);
    leftOut[root] = lc;                  // 0xFFFF if the half was empty
    rightOut[root] = rc;
    return root;
}

void
GpuClusterTopologyHelper::InstallCollectiveEmbedding(const std::string& family,
                                                     const std::vector<uint32_t>& dims)
{
    uint32_t N = m_endpoints.GetN();
    if (N < 2)
    {
        return;
    }

    // Gather endpoints and their (NodeId, LocalRankBase) metadata.
    std::vector<Ptr<FabricEndpoint>> eps(N);
    for (uint32_t i = 0; i < N; ++i)
    {
        eps[i] = DynamicCast<FabricEndpoint>(m_endpoints.Get(i));
    }

    // Build the locality-ordered permutation sortedOrder[i] = rank at ring pos i.
    // Consecutive entries are physically adjacent under the embedding.
    std::vector<uint16_t> sortedOrder(N);
    std::iota(sortedOrder.begin(), sortedOrder.end(), 0);

    if (family == "hypercube")
    {
        // Reflected Gray code: position i maps to rank i ^ (i>>1). Consecutive
        // positions (incl. wrap) differ in exactly one bit -> 1 physical hop.
        // N must be a power of 2 (true for hypercube builds).
        for (uint32_t i = 0; i < N; ++i)
        {
            sortedOrder[i] = static_cast<uint16_t>(i ^ (i >> 1));
        }
    }
    else if (family == "torus" && dims.size() >= 3)
    {
        // Serpentine walk over (x,y,z): flip direction per row so consecutive
        // ranks are physically adjacent (differ in one dimension, 1 hop).
        uint32_t dx = dims[0], dy = dims[1], dz = dims[2];
        sortedOrder.clear();
        for (uint32_t x = 0; x < dx; ++x)
        {
            for (uint32_t yy = 0; yy < dy; ++yy)
            {
                uint32_t y = (x % 2 == 0) ? yy : (dy - 1 - yy);
                for (uint32_t zz = 0; zz < dz; ++zz)
                {
                    uint32_t z = (y % 2 == 0) ? zz : (dz - 1 - zz);
                    sortedOrder.push_back(static_cast<uint16_t>(
                        x * dy * dz + y * dz + z));
                }
            }
        }
    }
    else if (family == "mesh2d" && dims.size() >= 2)
    {
        uint32_t r = dims[0], c = dims[1];
        sortedOrder.clear();
        for (uint32_t i = 0; i < r; ++i)
        {
            for (uint32_t jj = 0; jj < c; ++jj)
            {
                uint32_t j = (i % 2 == 0) ? jj : (c - 1 - jj);
                sortedOrder.push_back(static_cast<uint16_t>(i * c + j));
            }
        }
    }
    else if ((family == "2dfullmesh" || family == "ndfullmesh") && !dims.empty())
    {
        // Snake over the first two dimensions; consecutive ranks share a
        // row or column (direct link in the full-mesh rack).
        uint32_t r = dims[0];
        uint32_t c = dims.size() > 1 ? dims[1] : 1;
        sortedOrder.clear();
        for (uint32_t i = 0; i < r; ++i)
        {
            for (uint32_t jj = 0; jj < c; ++jj)
            {
                uint32_t j = (i % 2 == 0) ? jj : (c - 1 - jj);
                sortedOrder.push_back(static_cast<uint16_t>(i * c + j));
            }
        }
    }
    else if (family == "railfattree" && dims.size() >= 2)
    {
        const uint32_t rails = dims[0];
        const uint32_t nodesPerLeaf = dims[1];
        NS_ABORT_MSG_IF(rails == 0 || nodesPerLeaf == 0 || N % rails != 0,
                        "Invalid rail-optimized collective embedding");
        const uint32_t enclosures = N / rails;
        const uint32_t leavesPerRail =
            (enclosures + nodesPerLeaf - 1) / nodesPerLeaf;
        sortedOrder.clear();
        for (uint32_t rail = 0; rail < rails; ++rail)
        {
            for (uint32_t segment = 0; segment < leavesPerRail; ++segment)
            {
                const uint32_t first = segment * nodesPerLeaf;
                const uint32_t last = std::min(first + nodesPerLeaf, enclosures);
                for (uint32_t enclosure = first; enclosure < last; ++enclosure)
                {
                    sortedOrder.push_back(
                        static_cast<uint16_t>(enclosure * rails + rail));
                }
            }
        }
    }
    else if (family == "ring" || family == "fullmesh")
    {
        // Identity: ring order is already physical; full-mesh is all-adjacent.
    }
    else
    {
        // Switched / hierarchical (leafspine, fattree, dragonflyplus,
        // multiplane, 3levelhierarchical, nvl72, 2dfullmeshclos, switched):
        // sort by (NodeId, LocalRankBase) so same-node ranks are contiguous,
        // making most ring steps intra-node/intra-rack (1 hop).
        std::sort(sortedOrder.begin(), sortedOrder.end(),
                  [&eps](uint16_t a, uint16_t b) {
                      uint32_t na = eps[a] ? eps[a]->GetNodeId() : 0;
                      uint32_t nb = eps[b] ? eps[b]->GetNodeId() : 0;
                      if (na != nb) return na < nb;
                      return a < b;
                  });
    }

    // Install ring next/prev from the cyclic locality order.
    for (uint32_t i = 0; i < N; ++i)
    {
        uint16_t r = sortedOrder[i];
        uint16_t next = sortedOrder[(i + 1) % N];
        uint16_t prev = sortedOrder[(i + N - 1) % N];
        if (eps[r])
        {
            eps[r]->SetRingNeighbors(next, prev);
        }
    }

    // Install a balanced locality tree (leaves = adjacent pairs).
    std::vector<uint16_t> parentOut(N, 0xFFFF), leftOut(N, 0xFFFF), rightOut(N, 0xFFFF);
    (void)BuildLocalityTree(sortedOrder, 0, N, 0xFFFF, parentOut, leftOut, rightOut);
    for (uint32_t i = 0; i < N; ++i)
    {
        if (eps[i])
        {
            eps[i]->SetTreeNeighbors(parentOut[i], leftOut[i], rightOut[i]);
        }
    }
}

} // namespace ns3
