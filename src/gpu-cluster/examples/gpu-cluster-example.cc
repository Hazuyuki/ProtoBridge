/*
 * SPDX-License-Identifier: GPL-2.0-only
 * Author: GPU Cluster Simulator Team
 *
 * Simple test example for GPU cluster network simulation
 * Tests basic point-to-point topology without InternetStack
 */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/fabric-endpoint.h"
#include "ns3/nvswitch.h"
#include "ns3/gpu-cluster-helper.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("GpuClusterTestExample");

/**
 * @brief Simple application to generate traffic on GPU endpoint
 */
class GpuTrafficGenerator : public Application
{
public:
    static TypeId GetTypeId();

    GpuTrafficGenerator();
    ~GpuTrafficGenerator() override;

    void Setup(Ptr<FabricEndpoint> endpoint, uint16_t destRank, uint32_t packetSize, uint32_t numPackets, Time interval);

private:
    void StartApplication() override;
    void StopApplication() override;
    void SendPacket();

    Ptr<FabricEndpoint> m_endpoint;
    uint16_t m_destRank;
    uint32_t m_packetSize;
    uint32_t m_numPackets;
    Time m_interval;
    uint32_t m_packetsSent;
    EventId m_sendEvent;
};

NS_OBJECT_ENSURE_REGISTERED(GpuTrafficGenerator);

TypeId
GpuTrafficGenerator::GetTypeId()
{
    static TypeId tid = TypeId("ns3::GpuTrafficGenerator")
        .SetParent<Application>()
        .SetGroupName("GpuCluster")
        .AddConstructor<GpuTrafficGenerator>();
    return tid;
}

GpuTrafficGenerator::GpuTrafficGenerator()
    : m_destRank(0),
      m_packetSize(1024),
      m_numPackets(100),
      m_interval(MicroSeconds(10)),
      m_packetsSent(0)
{
}

GpuTrafficGenerator::~GpuTrafficGenerator()
{
}

void
GpuTrafficGenerator::Setup(Ptr<FabricEndpoint> endpoint, uint16_t destRank, uint32_t packetSize, uint32_t numPackets, Time interval)
{
    m_endpoint = endpoint;
    m_destRank = destRank;
    m_packetSize = packetSize;
    m_numPackets = numPackets;
    m_interval = interval;
}

void
GpuTrafficGenerator::StartApplication()
{
    m_packetsSent = 0;
    Simulator::Schedule(Seconds(0.1), &GpuTrafficGenerator::SendPacket, this);
}

void
GpuTrafficGenerator::StopApplication()
{
    if (m_sendEvent.IsPending())
    {
        Simulator::Cancel(m_sendEvent);
    }
}

void
GpuTrafficGenerator::SendPacket()
{
    if (m_packetsSent >= m_numPackets)
    {
        return;
    }

    if (m_endpoint)
    {
        std::vector<uint8_t> data(m_packetSize, 0xAB);
        m_endpoint->SendData(m_destRank, data.data(), m_packetSize, 1, 0);
        m_packetsSent++;
        NS_LOG_INFO("Sent packet " << m_packetsSent << " of " << m_numPackets);
    }

    m_sendEvent = Simulator::Schedule(m_interval, &GpuTrafficGenerator::SendPacket, this);
}

// Callback to handle received data
void
PacketReceivedCallback(uint16_t srcRank, Ptr<Packet> packet, FabricHeader header)
{
    NS_LOG_INFO("Received packet from rank " << srcRank << " size " << packet->GetSize() << " seq " << header.GetSequenceNumber());
}

int
main(int argc, char* argv[])
{
    bool verbose = true;
    uint32_t numGpus = 4;
    uint32_t packetSize = 1024;
    uint32_t numPackets = 10;
    std::string dataRate = "100Gbps";
    std::string delay = "500ns";

    CommandLine cmd;
    cmd.AddValue("verbose", "Enable verbose logging", verbose);
    cmd.AddValue("numGpus", "Number of GPUs to simulate", numGpus);
    cmd.AddValue("packetSize", "Size of packets to send", packetSize);
    cmd.AddValue("numPackets", "Number of packets to send", numPackets);
    cmd.AddValue("dataRate", "Data rate for links", dataRate);
    cmd.AddValue("delay", "Link delay", delay);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        LogComponentEnable("GpuClusterTestExample", LOG_LEVEL_INFO);
        LogComponentEnable("FabricEndpoint", LOG_LEVEL_LOGIC);
        LogComponentEnable("NvSwitch", LOG_LEVEL_LOGIC);
        LogComponentEnable("PointToPointNetDevice", LOG_LEVEL_LOGIC);
    }

    NS_LOG_INFO("Creating GPU cluster topology with " << numGpus << " GPUs");

    // Create topology using helper
    GpuClusterTopologyHelper topologyHelper(numGpus, 1);
    topologyHelper.SetLinkDataRate(dataRate);
    topologyHelper.SetLinkDelay(delay);

    NodeContainer gpuNodes = topologyHelper.BuildFullyConnected();
    ApplicationContainer endpoints = topologyHelper.GetGpuEndpoints();

    NS_LOG_INFO("Created " << gpuNodes.GetN() << " GPU nodes");

    // Configure endpoints
    for (uint32_t i = 0; i < endpoints.GetN(); ++i)
    {
        Ptr<FabricEndpoint> endpoint = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        if (endpoint)
        {
            endpoint->SetReceiveCallback(MakeCallback(&PacketReceivedCallback));
            endpoint->SetNumVirtualChannels(2);
            endpoint->SetVcCredits(0, 64);
            endpoint->SetVcCredits(1, 64);
        }
    }

    // Install traffic generator on GPU 0 to send to GPU 1
    Ptr<GpuTrafficGenerator> generator = CreateObject<GpuTrafficGenerator>();
    Ptr<FabricEndpoint> endpoint0 = DynamicCast<FabricEndpoint>(endpoints.Get(0));
    generator->Setup(endpoint0, 1, packetSize, numPackets, MicroSeconds(100));
    gpuNodes.Get(0)->AddApplication(generator);
    generator->SetStartTime(Seconds(1.0));
    generator->SetStopTime(Seconds(5.0));

    // Also install generator on GPU 2 to send to GPU 3
    Ptr<GpuTrafficGenerator> generator2 = CreateObject<GpuTrafficGenerator>();
    Ptr<FabricEndpoint> endpoint2 = DynamicCast<FabricEndpoint>(endpoints.Get(2));
    generator2->Setup(endpoint2, 3, packetSize, numPackets, MicroSeconds(100));
    gpuNodes.Get(2)->AddApplication(generator2);
    generator2->SetStartTime(Seconds(1.5));
    generator2->SetStopTime(Seconds(5.0));

    NS_LOG_INFO("Running simulation...");

    Simulator::Stop(Seconds(10.0));
    Simulator::Run();

    NS_LOG_INFO("Simulation completed");

    // Print statistics
    for (uint32_t i = 0; i < endpoints.GetN(); ++i)
    {
        Ptr<FabricEndpoint> endpoint = DynamicCast<FabricEndpoint>(endpoints.Get(i));
        if (endpoint)
        {
            NS_LOG_INFO("GPU " << i << " Rank " << endpoint->GetRank()
                      << " TX packets: " << endpoint->GetNNetDevices() << " devices");
        }
    }

    Simulator::Destroy();
    return 0;
}
