/* SPDX-License-Identifier: GPL-2.0-only
 * NICS-EFC: a manually wired RoCE topology. Inputs are illustrative, not fitted.
 *
 * GPU/NIC 0 ---\                  /--- GPU/NIC 2
 *               Leaf 0 -- Leaf 1
 * GPU/NIC 1 ---/                  \--- GPU/NIC 3
 *
 * Each endpoint represents a different host with one GPU/NIC pair. Two flows
 * (0->2 and 1->3) share the middle link. --reverse=1 reverses both flows.
 */
#include "ns3/command-line.h"
#include "ns3/double.h"
#include "ns3/ethernet-switch.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/gpu-cluster-helper.h"
#include "ns3/mac48-address.h"
#include "ns3/node-container.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/queue-size.h"
#include "ns3/rdma-submission-model.h"
#include "ns3/roce-payload-builder.h"
#include "ns3/roce-protocol-model.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/uinteger.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

using namespace ns3;

struct Progress
{
    uint64_t expectedBytes{0}, receivedBytes{0};
    uint64_t expectedWrites{0}, completedWrites{0};
    bool failed{false}, complete{false};
    Time started, finished;

    void Check()
    {
        if (!failed && receivedBytes == expectedBytes && completedWrites == expectedWrites)
        {
            complete = true;
            finished = Simulator::Now();
            Simulator::Stop();
        }
    }
};

static void
Received(Progress* progress, uint16_t, Ptr<Packet>, FabricHeader header)
{
    progress->receivedBytes += header.GetEffectiveDataSize();
    NS_ABORT_MSG_IF(progress->receivedBytes > progress->expectedBytes, "Duplicate payload");
    progress->Check();
}

static PointToPointHelper
MakeLink(const std::string& rate, uint32_t latencyNs)
{
    PointToPointHelper link;
    link.SetDeviceAttribute("DataRate", StringValue(rate));
    link.SetDeviceAttribute("Mtu", UintegerValue(65535));
    link.SetChannelAttribute("Delay", TimeValue(NanoSeconds(latencyNs)));
    link.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue(QueueSize("10000p")));
    return link;
}

int
main(int argc, char* argv[])
{
    std::string accessRate = "200Gbps", trunkRate = "100Gbps";
    std::string pcieModel = "bandwidth";
    uint32_t linkLatencyNs = 0, switchDelayNs = 649;
    uint32_t dataSize = 65536, iterations = 4;
    uint64_t stopUs = 100000;
    bool reverse = false;
    CommandLine cmd(__FILE__);
    cmd.AddValue("accessRate", "GPU/NIC-to-leaf rate", accessRate);
    cmd.AddValue("trunkRate", "Shared leaf-to-leaf rate", trunkRate);
    cmd.AddValue("linkLatencyNs", "Propagation delay of each network link", linkLatencyNs);
    cmd.AddValue("switchDelayNs", "Forwarding delay of each network switch", switchDelayNs);
    cmd.AddValue("dataSize", "Bytes per Write", dataSize);
    cmd.AddValue("iterations", "Writes per flow", iterations);
    cmd.AddValue("pcieModel", "Local PCIe: bandwidth or tlp", pcieModel);
    cmd.AddValue("reverse", "Use flows 2->0 and 3->1", reverse);
    cmd.AddValue("stopUs", "Simulation deadline in microseconds", stopUs);
    cmd.Parse(argc, argv);
    NS_ABORT_MSG_IF(!dataSize || dataSize > (64u << 20) || !iterations || iterations > 1024,
                    "Use 1..64MiB per Write and 1..1024 iterations");
    NS_ABORT_MSG_IF(linkLatencyNs > 1000000000 || switchDelayNs > 1000000000,
                    "This example limits individual delays to one second");
    NS_ABORT_MSG_IF(!stopUs || stopUs > 1000000000,
                    "Use a positive deadline of at most 1000 seconds");

    // 1. Nodes represent hosts and network switch devices.
    NodeContainer hosts, switchNodes;
    hosts.Create(4);
    switchNodes.Create(2);
    std::array<Ptr<EthernetSwitch>, 2> leaves;
    NvSwitchHelper switchHelper;
    switchHelper.SetSwitchType("ns3::EthernetSwitch");
    for (uint32_t s = 0; s < 2; ++s)
    {
        leaves[s] = DynamicCast<EthernetSwitch>(switchHelper.Install(switchNodes.Get(s)));
        leaves[s]->SetCutThroughDelay(switchDelayNs);
        leaves[s]->SetVoqDepth(10000);
        leaves[s]->SetAttribute("VoqDropThreshold", UintegerValue(0));
        leaves[s]->SetAttribute("EcnThreshold", UintegerValue(0));
    }

    // 2. Create four RoCE endpoints. A single-GPU host has no local peer ranks.
    std::array<Ptr<FabricEndpoint>, 4> endpoints;
    for (uint16_t rank = 0; rank < 4; ++rank)
    {
        auto ep = CreateObject<FabricEndpoint>();
        ep->SetRank(rank);
        ep->SetLocalRankRange(rank, 1);
        ep->SetFabricType(FabricType::ROCE);
        ep->SetLaunchDelay(0);
        ep->SetBypassReorderBuffer(true);
        auto protocol = CreateObject<RoceProtocolModel>();
        protocol->SetAttribute("WireEfficiency", DoubleValue(1.0));
        ep->SetProtocolModel(protocol);
        ep->SetProtocolPayloadBuilder(CreateObject<RocePayloadBuilder>());
        hosts.Get(rank)->AddApplication(ep);
        ep->SetStartTime(Seconds(0));
        endpoints[rank] = ep;
    }

    // 3. Wire physical links and retain the returned endpoint/port identifiers.
    auto access = MakeLink(accessRate, linkLatencyNs);
    auto trunk = MakeLink(trunkRate, linkLatencyNs);
    std::array<uint32_t, 4> localPorts, endpointDevices;
    std::array<Mac48Address, 4> macs;
    for (uint16_t rank = 0; rank < 4; ++rank)
    {
        uint32_t side = rank / 2;
        auto devices = access.Install(hosts.Get(rank), switchNodes.Get(side));
        endpointDevices[rank] = endpoints[rank]->AddNetDevice(devices.Get(0));
        localPorts[rank] = leaves[side]->AddPort(devices.Get(1));
        macs[rank] = Mac48Address::ConvertFrom(devices.Get(0)->GetAddress());
    }
    auto middle = trunk.Install(switchNodes.Get(0), switchNodes.Get(1));
    std::array<uint32_t, 2> trunkPorts = {leaves[0]->AddPort(middle.Get(0)),
                                          leaves[1]->AddPort(middle.Get(1))};

    // 4. Populate both endpoint routes and switch routes, in BOTH directions.
    // Endpoints select a device by destination rank. Switches select a port
    // by the final destination MAC; the middle switch is not the destination.
    for (uint16_t rank = 0; rank < 4; ++rank)
    {
        for (uint16_t peer = 0; peer < 4; ++peer)
        {
            if (rank != peer)
            {
                endpoints[rank]->SetRoutingEntry(peer, endpointDevices[rank]);
                endpoints[rank]->SetNeighborMac(peer, macs[peer]);
            }
        }
    }
    for (uint32_t side = 0; side < 2; ++side)
    {
        for (uint16_t dest = 0; dest < 4; ++dest)
        {
            uint32_t port = dest / 2 == side ? localPorts[dest] : trunkPorts[side];
            leaves[side]->AddStaticRoute(macs[dest], port);
        }
    }

    // 5. Attach detailed submission resources after devices/routes exist.
    // No fitted CPU/GPU delays or calibrated memory rates are assumed here.
    RdmaConfig rdma;
    rdma.initiator = "cpu";
    rdma.dataPath = "gpudirect";
    rdma.pcieModel = pcieModel;
    rdma.queueDepth = 16;
    rdma.Validate();
    for (auto ep : endpoints)
    {
        auto submission = CreateObject<RdmaSubmissionModel>();
        submission->Configure(rdma);
        submission->SetHostMemoryResources(std::make_shared<RdmaHostMemoryResources>());
        ep->SetRdmaSubmissionModel(submission);
        ep->Initialize();
    }

    Progress progress;
    progress.expectedBytes = 2ull * dataSize * iterations;
    progress.expectedWrites = 2ull * iterations;
    for (auto ep : endpoints)
    {
        ep->SetReceiveCallback(MakeBoundCallback(&Received, &progress));
    }
    Simulator::Schedule(MicroSeconds(1), [&] {
        progress.started = Simulator::Now();
        for (uint16_t flow = 0; flow < 2; ++flow)
        {
            uint16_t src = reverse ? flow + 2 : flow;
            uint16_t dst = reverse ? flow : flow + 2;
            for (uint32_t wr = 0; wr < iterations; ++wr)
            {
                endpoints[src]->SubmitRdmaWrite(dst, dataSize, 1, flow + 1, 0, 0, [&](bool ok) {
                    if (!ok)
                    {
                        progress.failed = true;
                        Simulator::Stop();
                        return;
                    }
                    ++progress.completedWrites;
                    progress.Check();
                });
            }
        }
    });
    Simulator::Stop(MicroSeconds(stopUs)); // Watchdog; success stops earlier.
    Simulator::Run();
    uint64_t durationNs =
        progress.complete ? (progress.finished - progress.started).GetNanoSeconds() : 0;
    std::cout << "topology=two-leaf-dumbbell\n"
              << "accessRate=" << accessRate << "\ntrunkRate=" << trunkRate
              << "\npcieModel=" << pcieModel << "\nreverse=" << reverse
              << "\nexpectedBytes=" << progress.expectedBytes
              << "\nreceivedBytes=" << progress.receivedBytes
              << "\ncompletedWrites=" << progress.completedWrites << "\ndurationNs=" << durationNs
              << "\naggregate_MBps="
              << (durationNs ? progress.receivedBytes * 1000.0 / durationNs : 0)
              << "\nstatus=" << (progress.complete ? "complete" : "incomplete") << '\n';
    Simulator::Destroy();
    return progress.complete ? 0 : 1;
}
