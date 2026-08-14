/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Ring Topology Example with Link Degradation
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/gpu-cluster-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("RingTopologyExample");

static uint32_t g_rxCount = 0;

void
PacketReceived(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    g_rxCount++;
    NS_LOG_INFO("RX from rank " << srcRank << " size " << packet->GetSize()
                << " seq " << header.GetSequenceNumber());
}

int
main(int argc, char* argv[])
{
    uint32_t numGpus = 4;
    std::string dataRate = "100Gbps";
    std::string delay = "100ns";
    bool useDegradation = false;

    CommandLine cmd;
    cmd.AddValue("numGpus", "Number of GPUs", numGpus);
    cmd.AddValue("dataRate", "Link data rate", dataRate);
    cmd.AddValue("delay", "Link delay", delay);
    cmd.AddValue("degradation", "Enable link degradation", useDegradation);
    cmd.Parse(argc, argv);

    LogComponentEnable("RingTopologyExample", LOG_LEVEL_INFO);

    NS_LOG_UNCOND("=== Ring Topology Example ===");
    NS_LOG_UNCOND("GPUs: " << numGpus << " Degradation: " << (useDegradation ? "on" : "off"));

    GpuClusterTopologyHelper cluster(numGpus, 0);
    cluster.SetLinkDataRate(dataRate);
    cluster.SetLinkDelay(delay);

    if (useDegradation)
    {
        Ptr<LinkDegradationModel> degModel = CreateObject<LinkDegradationModel>();
        degModel->SetPacketLossRate(0.1);  // 10% loss
        cluster.SetLinkDegradationModel(degModel);
    }

    NodeContainer nodes = cluster.BuildRing();
    ApplicationContainer endpoints = cluster.GetEndpoints();

    for (uint32_t i = 0; i < endpoints.GetN(); ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        if (ep)
        {
            ep->SetReceiveCallback(MakeCallback(&PacketReceived));
            ep->SetNumVirtualChannels(2);
            ep->SetVcCredits(0, 64);
            ep->SetVcCredits(1, 64);
        }
    }

    // GPU 0 sends P2P to GPU 2 (should route through shortest path)
    Ptr<P2pTraffic> p2p = CreateObject<P2pTraffic>();
    p2p->SetEndpoint(DynamicCast<FabricEndpoint>(endpoints.Get(0)));
    p2p->SetDestRank(2);
    p2p->SetPacketSize(1024);
    p2p->SetMaxPackets(10);
    p2p->SetInterval(MicroSeconds(100));
    Simulator::Schedule(Seconds(0.001), &P2pTraffic::Start, p2p);

    Simulator::Stop(Seconds(1.0));
    Simulator::Run();

    NS_LOG_UNCOND("\n=== Results ===");
    NS_LOG_UNCOND("Packets received: " << g_rxCount);

    for (uint32_t i = 0; i < endpoints.GetN(); ++i)
    {
        Ptr<FabricEndpoint> ep = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        if (ep)
        {
            NS_LOG_UNCOND("GPU " << i << " devices=" << ep->GetNNetDevices());
        }
    }

    Simulator::Destroy();
    return 0;
}
